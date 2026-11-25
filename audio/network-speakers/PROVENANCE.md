# Network audio provenance and licences

The APEX rule is **no (L)GPLv3**, not permissive-only. LGPL-2.1 may be
selected for the existing LGPL-2.1-or-later dependencies. MPL-2.0 is allowed.

- PipeWire 1.6.9, commit `8fa27cabdc6c0c1350c69c026af5850ef0af1e26`, MIT.
  `module-raop-discover.c` supplies the TXT/property mapping implemented in
  `NetworkSpeakerDiscovery.java`. `module-snapcast-discover.c` supplies the
  service types, protocol-simple parameters and JSON-RPC stream URI followed
  by `module-maton-snapcast.c`. The latter is a new MIT implementation of that
  integration, with partial-write handling, RPC response/error checking and
  connection/registration deadlines. The stock RAOP sink and protocol-simple
  modules are built unchanged. No Avahi library or daemon is linked.
- AOSP BoringSSL repository commit
  `0cd1f6a94b670e6b61af95a06e71e690ae8e3262` in `external/boringssl`,
  `src/` source directory. Apache-2.0 with retained ISC/OpenSSL component
  notices; the complete repository LICENSE is staged. The crypto archive is
  linked into RAOP; no OpenSSL/SSL shared library is required. The build
  verifies this existing checkout pin before compiling it outside Soong.
- New native transport/controller/check scripts: MIT. Bridge integration:
  Apache-2.0. Existing PipeWire/WirePlumber/GLib/ALSA/Lua dependencies and
  licences remain listed in `audio/pipewire-PROVENANCE.md`.
- Roc Toolkit is MPL-2.0 and is permitted by the corrected rule. Roc and its
  optional codecs were not added in this round; manual Roc modules remain an
  optional follow-up, rather than a licence exclusion.

BoringSSL exposes the legacy branch selected by PipeWire (OpenSSL API level
below 30000). Its used external symbols are `EVP_EncryptInit`,
`EVP_EncryptUpdate`, `EVP_aes_128_cbc`, `EVP_Digest`, `EVP_md5`,
`EVP_CIPHER_CTX_new`, `EVP_CIPHER_CTX_free`, `RSA_new`, `RSA_set0_key`,
`RSA_public_encrypt`, `RSA_free`, `BN_bin2bn`, `BN_free`, and
`ERR_print_errors_fp`. All resolved in the NDK shared-module link with
`--no-undefined`. `RAND_bytes` is included by upstream's headers but is not
called: random bytes come from `pw_getrandom`. OpenSSL 3 provider/OSSL_PARAM
symbols are in the excluded branch and are not required.

`bionic-fill.h` (MatonOS-dev/bionic-fill 4638ac47, MIT), force-included
into the whole PipeWire build by `tools/build-pipewire.sh`, maps the obsolete BSD
`index`/`rindex` to `strchr`/`strrchr` on bionic. BoringSSL does not include
`openssl/bn.h` from `rsa.h`; `audio/patches/0008-raop-include-openssl-bn.patch`
adds the explicit include. Static libc++/libc++abi use the NDK's
Apache-2.0 WITH LLVM-exception licence.
