# Host test for jinxed_box: jbox_storage.c as it is, in front of
# feckless_box_calls and feckless_umode's table and ext hook as they are,
# against feckless_box_calls' own stand-ins, over real files.
#
#   make -C components/jinxed_box/test FECKLESS=<feckless-libs at umode>
#
# The stand-ins are feckless's test/fake*; SRC follows its test Makefile's
# list (u-feckless-0055) and needs the same change when that one does.
#
# SPDX-License-Identifier: MIT

CC ?= cc
FECKLESS ?= ../../../../../feckless-libs-for-m5tab5
U := $(FECKLESS)/components/feckless_umode
B := $(FECKLESS)/components/feckless_box_calls
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Werror \
          -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
          -D'JINXED_BOX_ROOT="jbox_root"' \
          -I$(B)/test/fake -I../include -I../../../main -I$(B)/include -I$(U)/include
LDFLAGS := -fsanitize=address,undefined -lm
SRC := jbox_test.c ../src/jbox_storage.c ../src/jbox_calls.c \
       $(B)/src/box_calls.c $(B)/src/box_storage.c $(B)/src/box_audio.c $(B)/src/box_rtos.c \
       $(B)/src/box_codec.c $(B)/src/box_nvs.c \
       $(B)/test/fake_board.c $(B)/test/fake_storage.c $(B)/test/fake_audio.c \
       $(B)/test/fake_rtos.c $(B)/test/fake_drivers.c $(B)/test/fake_codec.c $(B)/test/fake_nvs.c \
       $(U)/src/umode_calls.c $(U)/src/umode_box.c $(U)/src/umode_run.c

all: run

jbox_test: $(SRC) ../include/jinxed_box.h ../jinxed_calls.txt
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: jbox_test
	rm -rf work && mkdir work && cd work && ASAN_OPTIONS=detect_leaks=1 ../jbox_test ../../jinxed_calls.txt
	rm -rf work

clean:
	rm -rf jbox_test work
.PHONY: all run clean
