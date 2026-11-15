/* SPDX-License-Identifier: MIT
 * boxlog's host test: boxlog_run() as one side would, into argv[1]. */
#include "boxlog.h"
int main(int argc, char **argv) { return argc == 2 && boxlog_run(argv[1]) == 0 ? 0 : 1; }
