#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
cc=${CC:-cc}
flags=(-std=gnu11 -O2 -g -Wall -Wextra -Werror)
adapt=0
if [[ ${1:-} == --test-memory-adapter ]]; then adapt=1; shift; fi
if (( $# )); then echo 'usage: build.sh [--test-memory-adapter]' >&2; exit 2; fi
mkdir -p out
"$cc" "${flags[@]}" -fPIC -c sysvipc.c -o out/sysvipc.o
ar rcs out/libmaton-sysvipc.a out/sysvipc.o
"$cc" "${flags[@]}" -shared -fPIC -pthread shm-helper.c -o out/libmaton-shm.so
"$cc" "${flags[@]}" -pthread tests/harness.c out/sysvipc.o -o out/harness
"$cc" "${flags[@]}" tests/workload.c -o out/workload
"$cc" "${flags[@]}" tests/shared-workload.c -o out/shared-workload
"$cc" "${flags[@]}" -pthread -Itests tests/shared-listeners.c out/sysvipc.o -o out/shared-listeners
"$cc" "${flags[@]}" tests/stop-workload.c -o out/stop-workload
"$cc" "${flags[@]}" -pthread tests/edge-workload.c -o out/edge-workload
"$cc" "${flags[@]}" tests/memory-probe.c -o out/memory-probe
"$cc" "${flags[@]}" -pthread tests/abi-test.c \
    -Wl,--wrap=ioctl,--wrap=process_vm_readv,--wrap=process_vm_writev -o out/abi-test
out/abi-test
transport=1
if ! out/memory-probe; then
    transport=0
    echo 'SKIP: production memory transport is denied in this execution environment.'
fi
harness="$PWD/out/harness"
if (( adapt )); then
    "$cc" "${flags[@]}" -pthread tests/harness.c out/sysvipc.o tests/memory-adapter.c \
        -Wl,--wrap=process_vm_readv,--wrap=process_vm_writev -o out/harness-adapted
    harness="$PWD/out/harness-adapted"
    transport=1
    echo 'TEST ONLY: using /proc/<pid>/mem adapter; process_vm transport remains unvalidated.'
fi
if (( transport )); then
    "$harness" "$PWD/out/workload" "$PWD/out/libmaton-shm.so"
    "$harness" "$PWD/out/edge-workload" "$PWD/out/libmaton-shm.so"
    "$harness" "$PWD/out/stop-workload" "$PWD/out/libmaton-shm.so" --stop
    "$PWD/out/shared-listeners" "$PWD/out/shared-workload"
fi
if "$cc" -m32 tests/multilib-probe.c -o out/multilib-probe >out/multilib.log 2>&1; then
    "$cc" -m32 "${flags[@]}" -shared -fPIC -pthread shm-helper.c -o out/libmaton-shm32.so
    "$cc" -m32 "${flags[@]}" tests/workload.c -o out/workload32
    multilib=1
else
    multilib=0
    echo 'SKIP: i386 glibc workload/helper: host multilib is unavailable (out/multilib.log).'
fi
if "$cc" -m32 -nostdlib -static -fno-pie -no-pie -fno-stack-protector "${flags[@]}" \
        tests/compat.c tests/compat-start.S -o out/compat; then
    "$cc" -m32 -nostdlib -static -fno-pie -no-pie -fno-stack-protector "${flags[@]}" \
        tests/compat-probe.c tests/compat-start.S -o out/compat-probe
    if out/compat-probe; then
        if (( transport )); then
            "$harness" "$PWD/out/compat"
            if (( multilib )); then "$harness" "$PWD/out/workload32" "$PWD/out/libmaton-shm32.so"; fi
        fi
    else
        echo 'SKIP: real ELF32 execution is forbidden by the outer sandbox; wire ABI tests passed.'
    fi
else
    echo 'SKIP: compiler cannot build freestanding ELF32 tests.'
fi
ltproot=${LTPROOT:-}
if [[ -z "$ltproot" ]]; then
    for candidate in /opt/ltp /usr/lib/ltp /usr/libexec/ltp; do
        if [[ -d "$candidate/testcases/bin" ]]; then ltproot=$candidate; break; fi
    done
fi
if [[ -n "$ltproot" ]] && (( transport )); then
    echo "Running installed LTP SysV workloads from $ltproot"
    found=0
    for test in "$ltproot"/testcases/bin/{semget,semop,semctl,semtimedop,shmget,shmat,shmdt,shmctl,msgget,msgsnd,msgrcv,msgctl}[0-9][0-9]; do
        [[ -x "$test" ]] || continue
        found=1
        "$harness" "$test" "$PWD/out/libmaton-shm.so"
    done
    if (( !found )); then echo 'SKIP: no installed LTP SysV case binaries found.'; fi
else
    echo 'SKIP: LTP is not installed (mirrored behaviors are listed in README.md).'
fi
ndk=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
compiler="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang"
if [[ ! -x "$compiler" ]]; then echo "NDK r30 compiler missing: $compiler" >&2; exit 1; fi
if ! [[ $(<"$ndk/source.properties") == *'Pkg.Revision = 30.'* ]]; then
    echo 'ANDROID_NDK must point to NDK r30.' >&2; exit 1
fi
"$compiler" "${flags[@]}" -fPIC -c sysvipc.c -o out/sysvipc-android.o
"$compiler" -shared out/sysvipc-android.o -o out/libmaton-sysvipc-android.so
echo 'PASS: host and NDK r30 x86_64-linux-android36 builds (-Wall -Wextra -Werror)'
