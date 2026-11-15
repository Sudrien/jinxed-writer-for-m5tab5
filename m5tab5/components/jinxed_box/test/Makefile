# Host test for jinxed_box: jbox_storage.c as it is, with
# feckless_box_calls and feckless_umode's table and ext hook as they are,
# against feckless_box_calls' own stand-ins for the board and storage,
# over real files.
#
#   make -C components/jinxed_box/test FECKLESS=<feckless-libs at umode>
#
# SPDX-License-Identifier: MIT

CC ?= cc
FECKLESS ?= ../../../../../feckless-libs-for-m5tab5
U := $(FECKLESS)/components/feckless_umode
B := $(FECKLESS)/components/feckless_box_calls
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Werror \
          -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
          -D'JINXED_BOX_ROOT="jbox_root"' \
          -Ifake -I$(B)/test/fake -I../include -I$(B)/include -I$(U)/include
LDFLAGS := -fsanitize=address,undefined -lm
SRC := jbox_test.c ../src/jbox_storage.c \
       $(B)/src/box_calls.c $(B)/src/box_storage.c $(B)/src/box_audio.c \
       $(B)/test/fake_board.c $(B)/test/fake_storage.c $(B)/test/fake_audio.c \
       $(U)/src/umode_calls.c $(U)/src/umode_box.c $(U)/src/umode_run.c

all: run

jbox_test: $(SRC) $(wildcard fake/*.h) ../include/jinxed_box.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: jbox_test
	rm -rf work && mkdir work && cd work && ASAN_OPTIONS=detect_leaks=1 ../jbox_test ../../answers.txt
	rm -rf work

clean:
	rm -rf jbox_test work
.PHONY: all run clean
