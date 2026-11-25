#!/usr/bin/env python3
"""Compile fork sources with existing platform generated headers; never invoke Soong."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--aosp', type=Path, required=True)
parser.add_argument('--ndk', type=Path, required=True)
args = parser.parse_args()
root = args.aosp.resolve()
intermediates = root / 'out/soong/.intermediates'
includes = [root / name for name in (
    'frameworks/native/libs/binder/ndk/include_ndk',
    'frameworks/native/libs/binder/ndk/include_cpp',
    'frameworks/native/libs/binder/ndk/include_platform',
    'system/libbase/include', 'system/logging/liblog/include',
    'system/core/libutils/include', 'system/libfmq/include', 'system/libfmq/base',
    'system/core/libcutils/include', 'hardware/libhardware/include',
    'hardware/interfaces/bluetooth/audio/utils/aidl_session',
    'system/core/libsystem/include', 'system/core/libutils/binder/include',
    'system/libhidl/base/include', 'external/googletest/googletest/include',
)]
for name in (
    'hardware/interfaces/bluetooth/audio/aidl/android.hardware.bluetooth.audio-V6-ndk-source',
    'hardware/interfaces/audio/aidl/android.hardware.audio.common-V5-ndk-source',
    'hardware/interfaces/common/aidl/android.hardware.common-V2-ndk-source',
    'hardware/interfaces/common/fmq/aidl/android.hardware.common.fmq-V1-ndk-source',
    'system/hardware/interfaces/media/android.media.audio.common.types-V5-ndk-source',
):
    includes.append(intermediates / name / 'gen/include')
for directory in includes:
    if not directory.is_dir():
        parser.error(f'missing platform include directory: {directory}')
compiler = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang++'
flags = [str(compiler), '-std=c++20', '-Wall', '-Werror', '-D__ANDROID_VENDOR__', '-DBACKEND_NDK']
flags += ['-I' + str(path) for path in includes]
sources = Path(__file__).resolve().parents[1]
# Keep all scratch in this worktree, remove it even on compile failure.
with tempfile.TemporaryDirectory(prefix='.compile-', dir=sources) as scratch:
    for source in sorted(sources.glob('*.cpp')):
        subprocess.run(flags + ['-c', str(source), '-o', str(Path(scratch) / (source.stem + '.o'))], check=True)
        print(f'PASS: {source.name}', flush=True)
    subprocess.run(flags + ['-c', str(sources / 'tests/FactoryTest.cpp'), '-o', str(Path(scratch) / 'FactoryTest.o')], check=True)
    print('PASS: FactoryTest.cpp (compiled only)', flush=True)
