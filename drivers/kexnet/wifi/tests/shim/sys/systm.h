/* host shim for the crypto self-test */
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/cdefs.h>
#define explicit_bzero(p, n) memset((p), 0, (n))
#define swap32(x) __builtin_bswap32(x)
#define swap64(x) __builtin_bswap64(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
