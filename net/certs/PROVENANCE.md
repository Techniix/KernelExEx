# Root certificate store

- `roots.pem` is the Mozilla CA bundle as packaged by certifi
  (https://github.com/certifi/python-certifi, `certifi/cacert.pem`),
  commit `ca446ca404ff3b962a0193f36e03d1eabb231968` (2026-11-09), 118 roots.
- Licence: the bundle is MPL-2.0 data (see certifi's LICENSE). It is
  installed as a separate data file, not compiled into GPL code.

KernelEx installs it as `%windir%\KernelEx\certs\roots.pem`. Each machine may
add local roots in `certs\local\*.pem`. The planned signed updater
(see `net/docs/ARCHITECTURE.md`) replaces `roots.pem` atomically after checking
an Ed25519 signature.
