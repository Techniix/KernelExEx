#!/bin/sh
# Build the host test program for the portable parts of kextls and winhttp.
# Usage: net/tests/host/build.sh [build-dir]
set -eu
NET=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-"$NET/../build-host"}
mkdir -p "$OUT/obj"
CC=${CC:-clang}
EPOCH=$(cat "$NET/certs/EPOCH")
CFLAGS="-O1 -g -std=c99 -D_DEFAULT_SOURCE -Wall -Wno-unused-function \
 -DMBEDTLS_CONFIG_FILE=\"kex_mbedtls_config.h\" -DKEXTLS_STATIC -DKEX_DIV_PREFIX \
 -DKEXTLS_TRUST_EPOCH=${EPOCH}LL \
 -I$NET/kextls/config -I$NET/kextls/include -I$NET/kextls/src -I$NET/rt -I$NET/winhttp \
 -I$NET/third_party/mbedtls/include -I$NET/third_party/mbedtls/library"

objs=""
for f in "$NET"/third_party/mbedtls/library/*.c; do
  o="$OUT/obj/mbed_$(basename "$f" .c).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$NET/kextls/config/kex_mbedtls_config.h" -nt "$o" ]; then
    $CC $CFLAGS -w -c "$f" -o "$o"
  fi
  objs="$objs $o"
done
for f in "$NET"/kextls/src/kex_entropy.c "$NET"/kextls/src/drbg.c "$NET"/kextls/src/kex_platform.c \
         "$NET"/kextls/src/truststore.c "$NET"/kextls/src/session.c \
         "$NET"/winhttp/whttp_core.c "$NET"/winhttp/whttp_sock.c \
         "$NET"/rt/w9xrt_host.c "$NET"/rt/w9xrt_div.c "$NET"/tests/host/test_net.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  $CC $CFLAGS -c "$f" -o "$o"
  objs="$objs $o"
done
$CC -o "$OUT/test_net" $objs -lpthread
echo "built $OUT/test_net"

# kexrt: portable sync core + address text functions
$CC -O1 -g -std=c99 -D_DEFAULT_SOURCE -Wall -I"$NET"/kexrt/src \
  "$NET"/kexrt/src/sync_core.c "$NET"/kexrt/src/inet.c "$NET"/tests/host/test_kexrt.c \
  -o "$OUT/test_kexrt" -lpthread
echo "built $OUT/test_kexrt"
