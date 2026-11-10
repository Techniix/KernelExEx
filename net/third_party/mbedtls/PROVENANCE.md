# Mbed TLS (vendored)

- Upstream: https://github.com/Mbed-TLS/mbedtls
- Tag: `mbedtls-3.6.7` (3.6 LTS branch, supported until March 2027)
- Commit: `068ff080b369adfac81509f9b57b2afabaf82dc5`
- Kept: `library/`, `include/`, `LICENSE`. The upstream `library/Makefile` and
  `library/CMakeLists.txt` were removed; nothing else was changed.
- Licence: Apache-2.0 OR GPL-2.0-or-later. KernelEx takes it under
  **GPL-2.0-or-later**, which is compatible with KernelEx's GPL-2.0.

The KernelEx build configuration is `net/kextls/config/kex_mbedtls_config.h`.
Upgrading means replacing this directory with a newer tag, updating this file,
and rerunning `net/tests` (host tests) and the Windows 9x VM tests.
