/*
 * kbd.c -- a USB boot keyboard, as key events. See kbd.h.
 *
 * One client task does everything that touches the host stack: it pumps
 * usb_host_client_handle_events(), opens and claims what the callback
 * reported, and tears down what went away. Transfer callbacks run inside
 * handle_events() on that same task, so the interrupt URB is resubmitted
 * from its own callback and nothing here needs a lock except the
 * new-device list the event callback writes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "kbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usbhost.h"

#include "hidrpt.h"

static const char *TAG = "kbd";

#define KBD_MAX         (4)     /* keyboards (interfaces) open at once */
#define PENDING_MAX     (8)
#define QUEUE_LEN       (64)

#define HID_CLASS               (0x03)
#define HID_SUBCLASS_BOOT       (0x01)
#define HID_PROTO_KEYBOARD      (0x01)
#define HID_REQ_SET_IDLE        (0x0A)
#define HID_REQ_SET_PROTOCOL    (0x0B)
#define HID_DESC_HID            (0x21)
#define HID_DESC_REPORT         (0x22)
#define REPORT_DESC_MAX         (1024)  /* larger ones are read this far */

typedef struct {
    bool                 used;
    bool                 gone;      /* device left; tear down when idle */
    bool                 inflight;  /* the interrupt URB is submitted */
    usb_device_handle_t  dev;
    uint8_t              itf;
    usb_transfer_t      *xfer;
    uint8_t              prev_mods;
    uint8_t              prev[6];
} kbd_ctx_t;

static usb_host_client_handle_t s_client;
static QueueHandle_t            s_queue;
static kbd_ctx_t                s_kbd[KBD_MAX];

/*
 * 0025: what each keyboard device is -- its name and IDs, logged when it
 * comes, and whether its HID report descriptors name keys only Apple's
 * keyboards have (hidrpt.h), logged once the last one is read. One per
 * device, not per interface.
 */
typedef struct {
    bool                 used;
    usb_device_handle_t  dev;
    char                 name[64];
    uint16_t             vid, pid;
    int                  reading;   /* report descriptors still to come */
    bool                 fn, globe;
} kbd_dev_t;

static kbd_dev_t s_dev[KBD_MAX];

static uint8_t      s_pending[PENDING_MAX];
static int          s_pending_n;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- reports ---------------------------------------------------------- */

static void post(uint8_t usage, uint8_t mods, bool down)
{
    const kbd_event_t ev = { .usage = usage, .mods = mods, .down = down };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; key 0x%02X dropped", usage);
    }
}

static bool held(const uint8_t set[6], uint8_t code)
{
    for (int i = 0; i < 6; i++) if (set[i] == code) return true;
    return false;
}

/*
 * Byte 0 modifiers, byte 1 reserved, bytes 2..7 the keys held now. A
 * level, re-sent while anything is down, so presses and releases are the
 * differences from the previous report. All-ones (ErrorRollOver) means
 * too many keys: hold the previous state rather than release everything.
 */
static void report(kbd_ctx_t *k, const uint8_t *d, int len)
{
    if (len < 8) return;
    const uint8_t mods = d[0];
    const uint8_t *now = &d[2];
    if (now[0] == 0x01) return;

    for (int i = 0; i < 6; i++) {
        if (k->prev[i] >= 0x04 && !held(now, k->prev[i])) post(k->prev[i], mods, false);
    }
    for (int i = 0; i < 6; i++) {
        if (now[i] >= 0x04 && !held(k->prev, now[i])) post(now[i], mods, true);
    }
    k->prev_mods = mods;
    memcpy(k->prev, now, 6);
}

static void xfer_cb(usb_transfer_t *t)
{
    kbd_ctx_t *k = t->context;
    k->inflight = false;

    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        report(k, t->data_buffer, t->actual_num_bytes);
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               t->status == USB_TRANSFER_STATUS_CANCELED) {
        k->gone = true;
        return;
    } else if (t->status == USB_TRANSFER_STATUS_STALL) {
        ESP_LOGW(TAG, "itf %u stalled; giving up on it", k->itf);
        k->gone = true;
        return;
    }
    /* TIMED_OUT, ERROR, OVERFLOW: try again. */
    if (!k->gone && usb_host_transfer_submit(t) == ESP_OK) k->inflight = true;
    else k->gone = true;
}

/* ---- control requests ------------------------------------------------- */

static void ctrl_cb(usb_transfer_t *t)
{
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        /* SET_IDLE is optional and plenty of keyboards stall it. */
        ESP_LOGD(TAG, "class request 0x%02X: status %d",
                 ((usb_setup_packet_t *)t->data_buffer)->bRequest, (int)t->status);
    }
    usb_host_transfer_free(t);
}

/* A class request to the interface, no data stage, fire and forget: the
 * completion comes back through handle_events() on this task, after the
 * caller has returned to it, and frees the transfer. */
static void class_request(usb_device_handle_t dev, uint8_t itf,
                          uint8_t req, uint16_t value)
{
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &t) != ESP_OK) return;

    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                       USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = req;
    s->wValue   = value;
    s->wIndex   = itf;
    s->wLength  = 0;

    t->device_handle    = dev;
    t->bEndpointAddress = 0;
    t->num_bytes        = sizeof(usb_setup_packet_t);
    t->callback         = ctrl_cb;
    t->context          = NULL;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) usb_host_transfer_free(t);
}

/* ---- what the keyboard is (0025) -------------------------------------- */

static kbd_dev_t *dev_rec(usb_device_handle_t dev)
{
    for (int i = 0; i < KBD_MAX; i++) if (s_dev[i].used && s_dev[i].dev == dev) return &s_dev[i];
    return NULL;
}

/* A USB string descriptor (UTF-16LE) as ASCII, '?' for the rest. */
static size_t str_ascii(const usb_str_desc_t *sd, char *out, size_t size)
{
    size_t n = 0;
    if (sd && sd->bLength >= 2) {
        const size_t chars = (size_t)(sd->bLength - 2) / 2;
        for (size_t i = 0; i < chars && n + 1 < size; i++) {
            const uint16_t c = sd->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = '\0';
    return n;
}

static void dev_summary(const kbd_dev_t *r)
{
    if (r->fn || r->globe) {
        ESP_LOGI(TAG, "%s (%04x:%04x): Apple-only keys:%s%s", r->name, r->vid, r->pid,
                 r->fn ? " fn" : "", r->globe ? " globe" : "");
    } else {
        ESP_LOGI(TAG, "%s (%04x:%04x): no Apple-only keys", r->name, r->vid, r->pid);
    }
}

static void desc_cb(usb_transfer_t *t)
{
    const uint8_t itf = (uint8_t)(uintptr_t)t->context;
    kbd_dev_t *r = dev_rec(t->device_handle);
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > (int)sizeof(usb_setup_packet_t)) {
        hidrpt_t h;
        const size_t n = (size_t)t->actual_num_bytes - sizeof(usb_setup_packet_t);
        hidrpt_scan(t->data_buffer + sizeof(usb_setup_packet_t), n, &h);
        ESP_LOGI(TAG, "itf %u: report descriptor %u bytes, %d items%s%s%s%s", itf, (unsigned)n, h.items,
                 h.fn ? ", Apple fn" : "", h.globe ? ", globe" : "",
                 h.vendor ? ", a vendor page" : "", h.ok ? "" : ", cut short");
        if (r) { r->fn |= h.fn; r->globe |= h.globe; }
    } else {
        ESP_LOGW(TAG, "itf %u: report descriptor not read (status %d)", itf, (int)t->status);
    }
    usb_host_transfer_free(t);
    if (r && --r->reading == 0) dev_summary(r);
}

/* GET_DESCRIPTOR(Report) for one interface, fire and forget, as
 * class_request() is. True if it went. */
static bool report_desc_request(usb_device_handle_t dev, uint8_t itf, uint16_t len)
{
    if (len > REPORT_DESC_MAX) len = REPORT_DESC_MAX;
    usb_transfer_t *t = NULL;
    /* Room to a whole 64-byte packet past what is asked for. */
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + ((len + 63u) & ~63u), 0, &t) != ESP_OK) {
        return false;
    }
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                       USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    s->wValue   = (uint16_t)(HID_DESC_REPORT << 8);
    s->wIndex   = itf;
    s->wLength  = len;

    t->device_handle    = dev;
    t->bEndpointAddress = 0;
    t->num_bytes        = (int)(sizeof(usb_setup_packet_t) + len);
    t->callback         = desc_cb;
    t->context          = (void *)(uintptr_t)itf;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
        usb_host_transfer_free(t);
        return false;
    }
    return true;
}

/*
 * The device's name and IDs, logged; then each HID interface's report
 * descriptor asked for -- every one, not only the boot keyboard's,
 * since an Fn key may be reported on another. The HID descriptor after
 * each interface gives the length, and the country code, which nearly
 * every keyboard leaves 0 ("not given").
 */
static void identify(usb_device_handle_t dev, const usb_config_desc_t *cfg)
{
    kbd_dev_t *r = NULL;
    for (int i = 0; i < KBD_MAX; i++) if (!s_dev[i].used) { r = &s_dev[i]; break; }
    if (!r) return;
    memset(r, 0, sizeof(*r));

    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd) {
        r->vid = dd->idVendor;
        r->pid = dd->idProduct;
    }
    usb_device_info_t info;
    char mfr[32] = "", prod[48] = "";
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        str_ascii(info.str_desc_manufacturer, mfr, sizeof mfr);
        str_ascii(info.str_desc_product, prod, sizeof prod);
    }
    if (mfr[0] && prod[0]) snprintf(r->name, sizeof r->name, "%.31s %.31s", mfr, prod);
    else if (mfr[0] || prod[0]) snprintf(r->name, sizeof r->name, "%s", mfr[0] ? mfr : prod);
    else snprintf(r->name, sizeof r->name, "unnamed keyboard");
    ESP_LOGI(TAG, "keyboard: %s (%04x:%04x)%s", r->name, r->vid, r->pid,
             r->vid == 0x05AC ? ", Apple's vendor ID" : "");
    r->used = true;
    r->dev  = dev;

    /* Walk the configuration: an interface, then its HID descriptor. */
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    int itf = -1;
    bool hid = false;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[0] >= 9) {
            const usb_intf_desc_t *id = (const usb_intf_desc_t *)p;
            hid = id->bAlternateSetting == 0 && id->bInterfaceClass == HID_CLASS;
            itf = id->bInterfaceNumber;
        } else if (p[1] == HID_DESC_HID && hid && p[0] >= 9 && p[6] == HID_DESC_REPORT) {
            const uint16_t len = (uint16_t)(p[7] | (p[8] << 8));
            ESP_LOGI(TAG, "itf %d: HID, country %u", itf, p[4]);
            if (len && report_desc_request(dev, (uint8_t)itf, len)) r->reading++;
            hid = false;
        }
        p += p[0];
    }
    if (r->reading == 0) dev_summary(r);
}

/* ---- open and close --------------------------------------------------- */

static kbd_ctx_t *slot(void)
{
    for (int i = 0; i < KBD_MAX; i++) if (!s_kbd[i].used) return &s_kbd[i];
    return NULL;
}

static bool dev_in_use(usb_device_handle_t dev)
{
    for (int i = 0; i < KBD_MAX; i++) if (s_kbd[i].used && s_kbd[i].dev == dev) return true;
    return false;
}

static bool claim(usb_device_handle_t dev, const usb_config_desc_t *cfg,
                  const usb_intf_desc_t *intf, int intf_off)
{
    const usb_ep_desc_t *ep = NULL;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        int off = intf_off;
        const usb_ep_desc_t *e =
            usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &off);
        if (e && USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR &&
            USB_EP_DESC_GET_EP_DIR(e)) {
            ep = e;
            break;
        }
    }
    if (!ep) return false;

    kbd_ctx_t *k = slot();
    if (!k) {
        ESP_LOGW(TAG, "more than %d keyboards; ignoring itf %u", KBD_MAX,
                 intf->bInterfaceNumber);
        return false;
    }
    if (usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber, 0) != ESP_OK) {
        ESP_LOGW(TAG, "itf %u: claim refused", intf->bInterfaceNumber);
        return false;
    }

    const uint16_t mps = USB_EP_DESC_GET_MPS(ep);
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(mps < 8 ? 8 : mps, 0, &t) != ESP_OK) {
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        return false;
    }

    memset(k, 0, sizeof(*k));
    k->used = true;
    k->dev  = dev;
    k->itf  = intf->bInterfaceNumber;
    k->xfer = t;
    t->device_handle    = dev;
    t->bEndpointAddress = ep->bEndpointAddress;
    t->num_bytes        = mps;
    t->callback         = xfer_cb;
    t->context          = k;

    /* Boot protocol, so the report is the 8 bytes report() reads; and
     * no idle repeats, since the state is diffed anyway. */
    class_request(dev, k->itf, HID_REQ_SET_PROTOCOL, 0);
    class_request(dev, k->itf, HID_REQ_SET_IDLE, 0);

    if (usb_host_transfer_submit(t) == ESP_OK) k->inflight = true;
    else k->gone = true;

    ESP_LOGI(TAG, "keyboard on itf %u, EP 0x%02X, %u ms", k->itf,
             ep->bEndpointAddress, ep->bInterval);
    return true;
}

static void inspect(usb_device_handle_t dev)
{
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return;

    bool any = false;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                                  &off)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        if (intf->bAlternateSetting == 0 &&
            intf->bInterfaceClass == HID_CLASS &&
            intf->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
            intf->bInterfaceProtocol == HID_PROTO_KEYBOARD) {
            any |= claim(dev, cfg, intf, off);
        }
    }
    if (!any) usb_host_device_close(s_client, dev);
    else identify(dev, cfg);
}

/* Once the URB is back: release, and close the device when its last
 * interface goes. Called only from the client task. */
static void reap(void)
{
    for (int i = 0; i < KBD_MAX; i++) {
        kbd_ctx_t *k = &s_kbd[i];
        if (!k->used || !k->gone) continue;
        if (k->inflight) {
            /* Ask for it back; the CANCELED completion clears inflight
             * on a later pass. */
            usb_host_endpoint_halt(k->dev, k->xfer->bEndpointAddress);
            usb_host_endpoint_flush(k->dev, k->xfer->bEndpointAddress);
            continue;
        }
        /* A report descriptor still being read holds the device: its
         * transfer comes back first, and frees itself (0025). */
        const kbd_dev_t *rd = dev_rec(k->dev);
        if (rd && rd->reading > 0) continue;
        /* Release every key it was holding, or a held key repeats for
         * ever on the editor's side. */
        for (int j = 0; j < 6; j++) if (k->prev[j] >= 0x04) post(k->prev[j], 0, false);

        usb_host_endpoint_clear(k->dev, k->xfer->bEndpointAddress);
        usb_host_transfer_free(k->xfer);
        usb_host_interface_release(s_client, k->dev, k->itf);
        usb_device_handle_t dev = k->dev;
        kbd_dev_t *r = dev_rec(dev);
        k->used = false;
        if (!dev_in_use(dev)) {
            if (r) r->used = false;
            usb_host_device_close(s_client, dev);
        }
        ESP_LOGI(TAG, "keyboard gone (itf %u)", k->itf);
    }
}

/* ---- the client ------------------------------------------------------- */

static void event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        /* Opening from inside the callback is outside the host API's
         * contract; queue the address for the task. */
        taskENTER_CRITICAL(&s_pending_lock);
        if (s_pending_n < PENDING_MAX) s_pending[s_pending_n++] = msg->new_dev.address;
        taskEXIT_CRITICAL(&s_pending_lock);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < KBD_MAX; i++) {
            if (s_kbd[i].used && s_kbd[i].dev == msg->dev_gone.dev_hdl) s_kbd[i].gone = true;
        }
    }
}

static bool pending_take(uint8_t *addr)
{
    bool got = false;
    taskENTER_CRITICAL(&s_pending_lock);
    if (s_pending_n > 0) {
        *addr = s_pending[0];
        memmove(s_pending, s_pending + 1, (size_t)(--s_pending_n));
        got = true;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    return got;
}

static void client_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* A bound, not a fault: the pending list and reap() need the loop
         * to come round even when nothing else is happening. */
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));

        uint8_t addr;
        while (pending_take(&addr)) {
            usb_device_handle_t dev = NULL;
            if (usb_host_device_open(s_client, addr, &dev) == ESP_OK) inspect(dev);
        }
        reap();
    }
}

/* On usbhost.c's bus task, after usb_host_install() and before VBUS. */
static esp_err_t install(void)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = event_cb, .callback_arg = NULL },
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&cfg, &s_client), TAG, "register");
    /* Above the editor (4): a report waiting behind a redraw is a key
     * that arrives late. */
    if (xTaskCreate(client_task, "kbd_client", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kbd_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(kbd_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    return usbhost_register_class("kbd", install);
}

bool kbd_get(kbd_event_t *ev, uint32_t ticks)
{
    return xQueueReceive(s_queue, ev, ticks) == pdTRUE;
}

bool kbd_present(void)
{
    for (int i = 0; i < KBD_MAX; i++) if (s_kbd[i].used && !s_kbd[i].gone) return true;
    return false;
}

bool kbd_apple(void)
{
    for (int i = 0; i < KBD_MAX; i++) {
        if (s_dev[i].used && dev_in_use(s_dev[i].dev) && (s_dev[i].fn || s_dev[i].globe)) return true;
    }
    return false;
}

/* ---- US ANSI ---------------------------------------------------------- */

/* 0x1E..0x38: 1 2 3 4 5 6 7 8 9 0, Enter Esc Bksp Tab Space, - = [ ]
 * backslash, non-US # (also a backslash), ; ' ` , . /  -- unshifted and shifted. 0 is no character. */
static const char ROW_LO[] = "1234567890\n\0\0\t -=[]\\\\;'`,./";
static const char ROW_HI[] = "!@#$%^&*()\n\0\0\t _+{}||:\"~<>?";

char kbd_ascii(uint8_t usage, uint8_t mods, bool caps)
{
    const bool shift = (mods & KBD_MOD_SHIFT) != 0;
    if (usage >= 0x04 && usage <= 0x1D) {
        const bool upper = shift != caps;
        return (char)((upper ? 'A' : 'a') + (usage - 0x04));
    }
    if (usage >= 0x1E && usage <= 0x38) {
        return (shift ? ROW_HI : ROW_LO)[usage - 0x1E];
    }
    if (usage == KBD_KP_ENTER) return '\n';
    return 0;
}
