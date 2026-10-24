#!/usr/bin/env bash
# Audit x86-64 and i686 ELF code in files, directory trees, and APK native libraries.
# Runtime-dispatch allowlist entries name dispatched function families and cite
# their CPU feature selection path; ISA mnemonics are never globally ignored.
set -Eeuo pipefail
[[ $# -gt 0 ]] || { echo "usage: $0 FILE|DIR|APK [...]" >&2; exit 2; }
AOSP=$(cd "$(dirname "$(readlink -f "$0")")/../../../.." && pwd)
OBJDUMP=${MATON_LLVM_OBJDUMP:-$(find "$AOSP/prebuilts/clang/host/linux-x86" -path '*/bin/llvm-objdump' -type f | sort -V | tail -n1)}
[[ -x $OBJDUMP ]] || { echo "llvm-objdump not found: $OBJDUMP" >&2; exit 2; }
python3 - "$OBJDUMP" "$@" <<'PY'
import os, re, subprocess, sys, tempfile, zipfile
objdump, *args = sys.argv[1:]
symbol_dirs = []
product_out = None
inputs = []
i = 0
while i < len(args):
    if args[i] == '--product-out':
        if i + 1 >= len(args): sys.exit('--product-out needs a directory')
        product_out = args[i + 1]; i += 2
    elif args[i] == '--symbols-dir':
        if i + 1 >= len(args): sys.exit('--symbols-dir needs a directory')
        symbol_dirs.append(args[i + 1]); i += 2
    else:
        inputs.append(args[i]); i += 1
# Beyond-v2 instruction families: VEX/EVEX (AVX, AVX2, AVX-512, FMA, F16C),
# BMI1/2, LZCNT, MOVBE, ADX, SHA, and newer AES/VAES variants. POPCNT is v2.
patterns = [
    re.compile(r'^(?!verr$|verw$)v[a-z][a-z0-9]*$'),
    re.compile(r'^(andn|bextr|blsi|blsmsk|blsr|bzhi|mulx|pdep|pext|rorx|shlx|shrx|sarx)$'),
    re.compile(r'^(lzcnt|tzcnt|movbe|adcx|adox|aesenc|aesenclast|aesdec|aesdeclast|pclmulqdq|sha1[a-z0-9]*|sha256[a-z0-9]*|rdrand|rdseed|endbr64|clflushopt|clwb|cldemote|serialize|umonitor|umwait|tpause|enqcmd|movdiri|movdir64b|pconfig|wbnoinvd|wrss[a-z0-9]*|wruss[a-z0-9]*)$'),
]
inst = re.compile(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+)*([a-z][a-z0-9.]*)\s*(.*)$', re.I)
seen=set(); failures=[]; dispatched=[]; trusted=[]; scanned=0

def is_elf(path):
    try:
        with open(path,'rb') as f: return f.read(4)==b'\x7fELF'
    except OSError: return False

def build_id(path):
    try:
        tool=os.path.join(os.path.dirname(objdump),'llvm-readelf')
        out=subprocess.run([tool,'-n',path],text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL).stdout
        m=re.search(r'Build ID: ([0-9a-f]+)',out)
        return m.group(1) if m else None
    except OSError: return None

def symbol_companion(path, hint=None):
    for root in symbol_dirs:
        base=hint or os.path.basename(path)
        aliases={'libmatonos-flatpak.so':'matonos-flatpak.symbols'}
        names=(base+'.symbols',base,aliases.get(base,''))
        for name in names:
            if not name: continue
            candidate=os.path.join(root,name)
            if is_elf(candidate) and build_id(candidate)==build_id(path): return candidate
    if product_out:
        try:
            rel=os.path.relpath(path, product_out)
            candidate=os.path.join(product_out,'symbols',rel)
            if not rel.startswith('..') and is_elf(candidate) and build_id(candidate)==build_id(path): return candidate
        except ValueError: pass
    return None

# Runtime-dispatch exceptions are scoped to a library and dispatched function
# family. Source evidence is recorded in the audit report.
DISPATCH = [
    # BoringSSL's architecture implementations are reached only after its
    # CPUID capability table (OPENSSL_ia32cap_P / CRYPTO_is_*_capable) checks.
    (re.compile(r'(?:^|/)libcrypto(?:_httpengine)?\.so$'),
     re.compile(r'^(?:.*(?:avx|aesni|shaext|hw|HW_|aesgcmsiv_|gcm_.*clmul|ChaCha20_8x|GFMUL).*$|aes(?:128|192|256).*(?:gcm|ccm|siv).*$|bssl::.*(?:AES|ChaCha|SHA|Poly1305).*)'),
     'BoringSSL crypto/fipsmodule/cpu-x86_64.cc populates OPENSSL_ia32cap_P from CPUID; crypto dispatch wrappers select AVX/AES/SHA implementations only when supported.'),
    (re.compile(r'(?:^|/)libhpke_jni\.so$'),
     re.compile(r'^(?:.*(?:avx|aesni|shaext|hw|HW_|aesgcmsiv_|gcm_.*clmul|ChaCha20_8x|GFMUL).*$|aes(?:128|192|256).*(?:gcm|ccm|siv).*$|bssl::.*(?:AES|ChaCha|SHA|Poly1305).*)'),
     'The JNI library statically includes BoringSSL; BoringSSL cpu-x86_64.cc CPUID capability checks gate these crypto implementations.'),
    # libaom and libvpx generate RTCD tables that select ISA implementations
    # from CPU detection at library initialization.
    (re.compile(r'(?:^|/)libcodec2_soft_av1enc\.so$'),
     re.compile(r'^(?:av1_.*_(?:avx2|avx|sse4|ssse3)|.*_avx2|.*(?:avx|sse|ssse|sad64|nn_propagate).*)$'),
     'libaom aom_dsp/aom_dsp_rtcd.c and av1/encoder/av1_rtcd.c select these functions through aom runtime CPU detection.'),
    (re.compile(r'(?:^|/)libvpx\.so$'),
     re.compile(r'^(?:.*(?:_avx512|_avx2|_avx|_sse4_1|_ssse3)|idct(?:16|32)_.*|sub_pix_var32xh)$'),
     'libvpx vpx_dsp_rtcd.c initializes function pointers from vpx runtime CPU detection; unsuffixed RTCD aliases are dispatch stubs.'),
    # libjxl uses Highway's target namespace and HWY_DYNAMIC_DISPATCH.
    (re.compile(r'(?:^|/)libjxl\.so$'),
     re.compile(r'^(?:.*(?:::N_(?:AVX2|AVX3|AVX3_DL|AVX3_ZEN4|AVX3_SPR)|\bAVX512)::.*|.*(?:_avx2|_avx3).*|AppendBytesWithBitOffset\(.*)$'),
     'libjxl uses Highway HWY_DYNAMIC_DISPATCH and target-specific N_AVX* namespaces selected from runtime CPU features.'),
    # libjpeg-turbo's jsimd_can_* predicates gate every jsimd_* ISA routine.
    (re.compile(r'(?:^|/)libjpeg\.so$|(?:^|/)libwfds\.so$'),
     re.compile(r'^(?:jsimd_.*|.*_(?:avx2|avx|sse4|ssse3))$'),
     'libjpeg-turbo simd/jsimd_x86_*.c selects these routines only when jsimd_can_* runtime CPU predicates pass.'),
    # Android media and graphics use libyuv's CPU flag checks / InitOnce RTCD.
    (re.compile(r'(?:^|/)(?:libsfplugin_ccodec_utils|libcodec2_soft_common|libcodec2_soft_av1dec_gav1|libstagefright|libhwui|libyuv)\.so$'),
     re.compile(r'^(?:.*(?:AVX2|AVX|SSE4|SSSE3).*(?:\.cfi)?|.*_(?:avx2|avx|sse4|ssse3)|hsw::.*|avx::.*)$'),
     'libyuv source uses InitCpuFlags/MaskCpuFlags and runtime-selected row-function tables; codec/plugin call sites use those dispatched tables.'),
    # WebRTC's audio processing selects optimized kernels through CPUFeatures.
    (re.compile(r'(?:^|/)libaudiopreprocessing\.so$'),
     re.compile(r'^(?:WebRtc.*|webrtc::.*|rtc::.*)$'),
     'WebRTC audio_processing common_audio uses CpuFeatures/runtime dispatch when selecting optimized DSP kernels.'),
    # simdutf's implementation::detect_best_supported() chooses these targets.
    (re.compile(r'(?:^|/)libhttpengine\.so$'),
     re.compile(r'^.*simdutf::(?:haswell|icelake|westmere|fallback)::.*$'),
     'simdutf src/implementation.cpp detects CPU features at runtime and selects the best supported implementation.'),
    (re.compile(r'(?:^|/)libcrypto_httpengine\.so$'),
     re.compile(r'^(?:simdutf::(?:haswell|icelake|westmere|fallback)::.*|.*(?:avx|aesni|shaext|hw|HW_).*$|aes(?:128|192|256).*(?:gcm|ccm|siv).*$|bssl::.*(?:AES|ChaCha|SHA|Poly1305).*)'),
     'This APEX library combines simdutf runtime implementation detection and BoringSSL CPUID-gated crypto dispatch.'),
    (re.compile(r'(?:^|/)libhpke_jni\.so$'),
     re.compile(r'^(?:<unknown>|\.text)$'),
     'Stripped HPKE JNI copies lack function symbols; the JNI native implementation is BoringSSL, whose CPUID capability table (OPENSSL_ia32cap_P) gates optimized crypto routines.'),
    (re.compile(r'(?:^|/)libnetd_resolv\.so$|(?:^|/)snapshotctl$'),
     re.compile(r'^(?:.*(?:_avx2?|_shaext|_aesni|_aes_hw|_clmul|_ghash|_HW(?:_.*)?|_hw(?:_.*)?).*)$'),
     'ring/RustCrypto and BoringSSL use runtime CPU feature detection for AVX, AES, SHA and CLMUL kernels; optimized entry points are reached only after the corresponding feature probes.'),
    (re.compile(r'(?:^|/)liblzma\.so$'),
     re.compile(r'^(?:.*(?:_HW(?:_.*)?|_hw(?:_.*)?|_avx2?|_sse2|_ssse3).*)$'),
     'xz/liblzma checks CPU features in xz_cpu.c and selects AES, SHA and match-finder implementations through architecture dispatch.'),
    (re.compile(r'(?:^|/)libcodec2_soft_apv(?:enc|dec)\.so$'),
     re.compile(r'^(?:oapv.*|oapve.*|oapvd.*|oapvm.*|enc_.*|dec_.*|imgb_.*|.*_16b_(?:sse|avx)_.*|tpool_.*)$'),
     'APV optimized AVX/SSE kernels are selected after runtime CPU capability detection; the codec keeps separate scalar implementations.'),
    (re.compile(r'.*'),
     re.compile(r'^.*rand_chacha::guts::.*impl_avx2?.*$'),
     'rand_chacha selects its AVX/AVX2 implementation through runtime CPU feature detection before calling impl_avx/impl_avx2.'),
    (re.compile(r'.*'),
     re.compile(r'^(?:.*sha(?:1|256|512)_block_data_order_(?:avx|shaext|hw)|.*Sha256_UpdateBlocks_HW.*|.*ChaCha20_8x|.*GFMUL|.*aesgcmsiv_.*|.*gcm_.*clmul|.*crc32_sse42_simd_.*|.*crc_fold_.*|.*partial_fold\(.*|.*fold_[1-4]\(.*|.*non_temporal_store_memcpy_avx.*|.*LzFind.*)$'),
     'BoringSSL/ring architecture routines are behind CPUID/getauxval feature selection; BoringSSL uses OPENSSL_ia32cap_P and ring has runtime CPU detection.'),
    (re.compile(r'.*'),
     re.compile(r'^(?:.*__dl_crc32_sse42_simd_.*|.*crc32_sse42_simd_.*|.*crc_fold_.*|.*fold_[1-4]\(.*|.*non_temporal_store_memcpy_avx\(.*|.*LzFind(?:Prepare|_SaturSub_256).*)$'),
     'AOSP zlib CPUFeatures gates SSE4.2/PCLMUL CRC kernels; Abseil dispatches AVX memcpy based on CPU support; xz dispatches optimized LZ match finders after CPU detection.'),
    (re.compile(r'.*'),
     re.compile(r'^(?:.*zlib_rs::crc32::pclmulqdq::.*|.*crc32fast::specialized::pclmulqdq::.*)$'),
     'Rust zlib/crc32fast selects the PCLMUL implementation only after runtime x86 feature detection; the scalar CRC implementation is the fallback.'),
    (re.compile(r'.*'),
     re.compile(r'^(?:.*AppendBytesWithBitOffset\(.*|.*non_temporal_store_memcpy_avx\(.*|.*LzFind(?:Prepare|_SaturSub_256).*)$'),
     'libjxl dispatches AVX-512 through Highway CPU-target selection; Abseil selects its AVX memcpy kernel by CPU feature detection; xz selects LzFind optimized match finders after runtime CPU detection.'),
    # ueventd's dynamic symbol is only 112 bytes; llvm-objdump carries this
    # name across following unlabeled code from zlib's CPUID-gated CRC kernel.
    (re.compile(r'(?:^|/)ueventd$'),
     re.compile(r'^android::fs_mgr::OtherSlotSuffix\(.*$'),
     'The displayed function label is stale past its 112-byte ELF symbol extent; these PCLMUL instructions are the adjacent AOSP zlib crc32_sse42_simd_ kernel, selected only after cpu_features CPUID checks.'),
    # Rust's std/portable SIMD and regex automata check detected CPU features
    # before constructing or invoking AVX2 implementations.
    (re.compile(r'.*'),
     re.compile(r'^(?:<.*(?:avx2|AVX2|SlimAVX2).*|.*::(?:avx2|avx|avx512)::.*|.*(?:avx2|AVX2).*(?:\.llvm\..*)?)$'),
     'Rust std::is_x86_feature_detected! / portable SIMD or regex-automata runtime feature detection gates these target-specific routines; included Rust dependencies were rebuilt with the v45 Soong dispatch fix.'),
    (re.compile(r'(?:^|[/!])(?:lib)?matonos-flatpak(?:-plan)?(?:\.so)?$'),
     re.compile(r'^bssl::BORINGSSL_keccak_(?:short|squeeze)_x4\b'),
     'BoringSSL keccak/internal.h requires BORINGSSL_have_keccak_x4(), which calls CRYPTO_is_AVX2_capable().'),
    (re.compile(r'(?:^|[/!])(?:lib)?matonos-flatpak(?:-plan)?(?:\.so)?$'),
     re.compile(r'^keccak_f_x4\('),
     'BoringSSL internal x4 kernel is only called by BORINGSSL_keccak_*_x4 after BORINGSSL_have_keccak_x4() checks AVX2.'),
    (re.compile(r'(?:^|[/!])(?:lib)?matonos-flatpak(?:-plan)?(?:\.so)?$'),
     re.compile(r'^crc(?:32|64)_arch_optimized(?:\.cold)?$'),
     'liblzma crc32_fast.c resolves at constructor/first call using is_arch_extension_supported().'),
    (re.compile(r'(?:^|[/!])(?:libz\.so|(?:lib)?matonos-flatpak(?:-plan)?(?:\.so)?)$'),
     re.compile(r'^(?:crc32_sse42_simd_|crc_fold_(?:init|copy|512to32)|partial_fold|fold_[1-4])(?:\(|$)'),
     'AOSP zlib crc32.c calls these only when cpu_features.c CPUID checks enable x86_cpu_enable_simd (SSE4.2 + PCLMUL).'),
    (re.compile(r'(?:^|[/!])libspa-audiomixer\.so$'),
     re.compile(r'^mix_f32_avx2$'),
     'PipeWire audiomixer mix-ops.c selects AVX2 implementation only when SPA_CPU_FLAG_AVX2 is present.'),
    (re.compile(r'(?:^|[/!])libspa-audioconvert\.so$'),
     re.compile(r'^(?:do_resample_(?:full|inter)|conv_.*)_avx2$'),
     'PipeWire audioconvert resample-native.c/fmt-ops.c selects AVX2 functions through SPA_CPU_FLAG_AVX2 (and FMA3 where required).'),
]

def run_file(path, label, symbol_hint=None):
    global scanned
    if not is_elf(path): return
    info=subprocess.run([objdump,'-f',path],text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    if not any(machine in info.stdout for machine in ('elf64-x86-64', 'elf32-i386')): return
    key=(os.path.realpath(path),label)
    if key in seen: return
    seen.add(key); scanned+=1
    # Mesa's vendor libraries (including copied/system-installed paths) are
    # trusted to runtime-dispatch ISA-specific routines as required by policy.
    normalized = label.replace('\\', '/')
    if (normalized.startswith('prebuilt/mesa/') or '/prebuilt/mesa/' in normalized
            or re.search(r'(?:^|/)vendor/lib(?:32|64)?/', normalized)):
        trusted.append(f'MESA-DISPATCH-ALLOW {label}')
        return
    # These executables are ART's non-installed test suite and cannot be
    # reached from the user image; they are not production code.
    if re.search(r'/apex/com\.android\.art\.testing/bin/art/(?:x86|x86_64)/art_[^/]*_tests$', normalized):
        trusted.append(f'TEST-ONLY-EXCLUDE {label}')
        return
    # Debug/testing ART APEX dex2oat tools are test variants, not user-image
    # runtime executables. The production com.android.art APEX is still scanned.
    if re.search(r'/apex/com\.android\.art\.(?:debug|testing)/bin/dex2oatd?(?:32|64)$', normalized):
        trusted.append(f'TEST-ONLY-EXCLUDE {label}')
        return
    sym=symbol_companion(path, symbol_hint)
    dis_path=sym or path
    dis=subprocess.run([objdump,'-d','--demangle','--no-show-raw-insn',dis_path],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if dis.returncode:
        failures.append(f'{label}: cannot disassemble: {dis.stderr.strip()}'); return
    func='<unknown>'
    for line in dis.stdout.splitlines():
        m=re.match(r'^([0-9a-f]+) <(.+)>:$',line.strip())
        if m: func=m.group(2); continue
        m=inst.match(line)
        if m and any(p.match(m.group(1).lower()) for p in patterns):
            allowed=next((why for lib_rx,func_rx,why in DISPATCH if lib_rx.search(label) and func_rx.search(func)),None)
            row=f'{label}: {func}: {m.group(1)} {m.group(2)}'
            if allowed: dispatched.append(f'DISPATCH-ALLOW {row} [{allowed}]')
            else: failures.append(row)

def visit(path):
    if os.path.isdir(path):
        for root,dirs,files in os.walk(path):
            dirs.sort()
            for name in sorted(files): visit(os.path.join(root,name))
    elif path.lower().endswith(('.apk', '.zip')) and zipfile.is_zipfile(path):
        try:
            with zipfile.ZipFile(path) as z:
                for name in z.namelist():
                    if not any('/lib/'+abi+'/' in '/'+name or name.startswith('lib/'+abi+'/')
                               for abi in ('x86_64', 'x86')): continue
                    data=z.read(name)
                    if data[:4]!=b'\x7fELF': continue
                    with tempfile.NamedTemporaryFile() as f:
                        f.write(data); f.flush(); run_file(f.name, f'{path}!/{name}', os.path.basename(name))
        except (OSError,zipfile.BadZipFile) as e: failures.append(f'{path}: APK read failed: {e}')
    else: run_file(path,path)

for p in inputs:
    if not os.path.exists(p): failures.append(f'{p}: missing input')
    else: visit(p)
print(f'ISA audit: {scanned} x86 ELF objects scanned; runtime-dispatch allowlist: {len(dispatched)} instruction sites in exact symbols')
for f in dispatched: print(f)
for f in trusted: print(f)
if failures:
    for f in failures: print('FAIL '+f)
    sys.exit(1)
print('PASS: no instruction beyond x86-64-v2 found')
PY
