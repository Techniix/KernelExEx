#!/bin/sh
# Host tests for the touchpad core (drivers/input/touch), under ASan/UBSan.
set -e
d=$(dirname "$0")
t=../../drivers/input/touch
cd "$d"
cc -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I$t test_tp.c $t/tp_core.c $t/tp_alps.c $t/tp_syn.c $t/tp_elan.c -o /tmp/test_tp
/tmp/test_tp
