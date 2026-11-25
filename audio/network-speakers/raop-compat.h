/* SPDX-License-Identifier: MIT
 * Forced only for the RAOP target through its OpenSSL dependency Cflags.
 * BoringSSL deliberately doesn't include bn.h transitively from rsa.h.
 * Bionic removed the obsolete BSD index/rindex aliases; preserve semantics.
 */
#pragma once
#include <openssl/bn.h>
#include <string.h>
#ifdef __ANDROID__
#define index(s, c) strchr((s), (c))
#define rindex(s, c) strrchr((s), (c))
#endif
