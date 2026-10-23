#!/usr/bin/env bash
# Audit x86-64 and i686 ELF code in files, directory trees, and APK native libraries.
# Runtime-dispatch allowlist entries name exact functions and cite their
# CPUID selection path; broad library or mnemonic suppressions are forbidden.
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

# Runtime-dispatch exceptions are exact function names, never an entire library
# or instruction family. Source evidence is recorded in the audit report.
DISPATCH = [
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
    # Mesa is explicitly trusted to runtime-dispatch ISA-specific routines;
    # the vendor build includes its CPU feature checks and per-driver kernels.
    normalized = label.replace('\\', '/')
    if normalized.startswith('prebuilt/mesa/') or '/prebuilt/mesa/' in normalized:
        trusted.append(f'MESA-DISPATCH-ALLOW {label}')
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
    elif zipfile.is_zipfile(path):
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
