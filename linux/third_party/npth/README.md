# nPth

Pinned official release `npth-1.8.tar.bz2`, SHA-256
`8bd24b4f23a3065d6e5b26e98aba9ce783ea4fd781069c1b35d149694e90ca3e`. The NDK
r30/API 35 x86_64 build uses patch
`patches/0001-bionic-probe-pthread-create.patch`: bionic has pthread creation
but omits `pthread_cancel`, which upstream used only to detect whether pthread
support exists. License: LGPL-2.1-or-later.
