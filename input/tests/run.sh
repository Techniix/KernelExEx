#!/bin/sh
# Host tests for input/ (gamepad mapping engine)
set -e
out=${1:-/tmp}
cc -O2 -Wall -Wextra -o "$out/test_kxpad" "$(dirname "$0")/test_kxpad.c" "$(dirname "$0")/../xinput/kxpad.c"
"$out/test_kxpad"
