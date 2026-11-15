# Host test for boxlog (u-jinxed-0005): the probe as it goes into the
# .bin and the ELF, built twice as those two are -- DOCLOG_DIR the host's
# and the box's -- over notes doclog itself made, under ASan and UBSan.
# The two result files must be the same, the synthetic note must replay
# as appended, and the notes it read from must be untouched.
#
#   make -C boxlog/test
#
# SPDX-License-Identifier: MIT

CC ?= cc
M := ../../main
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Werror \
          -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
          -I../main -I$(M) -Ifake -DDOCLOG_HOST -DBOXLOG_HOST -D'BOXLOG_SRC="sd/jinxed"'
LDFLAGS := -fsanitize=address,undefined
SRC := run_probe.c ../main/boxlog.c $(M)/doclog.c $(M)/md.c

all: run

probe_host: $(SRC) ../main/boxlog.h fake/esp_err.h
	$(CC) $(CFLAGS) -D'DOCLOG_DIR="sd/jinxed/boxlog/host"' -o $@ $(SRC) $(LDFLAGS)
probe_box: $(SRC) ../main/boxlog.h fake/esp_err.h
	$(CC) $(CFLAGS) -D'DOCLOG_DIR="sd/jinxed/boxlog/box"' -o $@ $(SRC) $(LDFLAGS)
mknotes: mknotes.c $(M)/doclog.c fake/esp_err.h
	$(CC) $(CFLAGS) -D'DOCLOG_DIR="sd/jinxed"' -o $@ mknotes.c $(M)/doclog.c $(LDFLAGS)

run: probe_host probe_box mknotes
	rm -rf work && mkdir -p work/sd/jinxed
	cd work && ../mknotes 2>/dev/null
	cd work && tar cf before.tar sd/jinxed/.[!.]*
	cd work && ../probe_host sd/jinxed/boxlog/host.txt 2>/dev/null
	cd work && ../probe_box sd/jinxed/boxlog/box.txt 2>/dev/null
	cd work && ../probe_box sd/jinxed/boxlog/box2.txt 2>/dev/null
	cd work && cat sd/jinxed/boxlog/host.txt
	cd work && cmp sd/jinxed/boxlog/host.txt sd/jinxed/boxlog/box.txt
	cd work && cmp sd/jinxed/boxlog/box.txt sd/jinxed/boxlog/box2.txt
	cd work && grep -q "replay same export ok" sd/jinxed/boxlog/host.txt
	cd work && grep -q "^notes 4$$" sd/jinxed/boxlog/host.txt
	cd work && ! grep -qE "open error|copy failed|open failed|FAILED|DIFFERS" sd/jinxed/boxlog/host.txt
	cd work && mkdir chk && tar xf before.tar -C chk && for f in chk/sd/jinxed/.[!.]*; do cmp $$f sd/jinxed/$${f#chk/sd/jinxed/}; done
	rm -rf work
	@echo "boxlog: all passed"

clean:
	rm -rf probe_host probe_box mknotes work
.PHONY: all run clean
