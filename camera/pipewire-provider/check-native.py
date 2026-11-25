#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess,sys
import argparse
parser = argparse.ArgumentParser(description="Compile the PipeWire camera fork without Soong")
parser.add_argument('--aosp', required=True, type=Path)
parser.add_argument('--ndk', required=True, type=Path)
parser.add_argument('--pipewire-prefix', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
args = parser.parse_args()
N=args.ndk.resolve()
A=args.aosp.resolve(); O=A/'out/soong/.intermediates'; P=args.pipewire_prefix.resolve()/'include'
output=args.out.resolve(); output.mkdir(parents=True, exist_ok=True)
repo=Path(__file__).resolve().parents[2]
import os
os.chdir(repo)
roots=[A/'hardware/interfaces'/p/'aidl/aidl_api'/package/version for p,package,version in [
('camera/common','android.hardware.camera.common','1'),('camera/device','android.hardware.camera.device','1'),
('camera/provider','android.hardware.camera.provider','1'),('camera/metadata','android.hardware.camera.metadata','1'),
('common','android.hardware.common','2'),('common/fmq','android.hardware.common.fmq','1'),
('graphics/common','android.hardware.graphics.common','7')]]
out=output/'camera-aidl';(out/'include').mkdir(parents=True,exist_ok=True)
for root in roots[:3]:
 for source in root.rglob('*.aidl'):
  subprocess.run([str(A/'prebuilts/build-tools/linux-x86/bin/aidl'),'--lang=ndk','--structured','--stability=vintf','--version=1','--hash='+ (root/'.hash').read_text().strip(),'--out='+str(out),'--header_out='+str(out/'include'),*['-I'+str(r) for r in roots],str(source)],check=True)

inc=['camera/pipewire-provider/src','native/third_party/binder_ndk_cpp',
str(P/'pipewire-0.3'),str(P/'spa-0.2')]
paths=['hardware/interfaces/common/support/include','frameworks/native/libs/gralloc/types/include','system/libfmq/base','hardware/interfaces/camera/common/default/include','hardware/interfaces/camera/metadata/3.2/types.h',
'frameworks/native/libs/binder/ndk/include_platform','frameworks/native/libs/binder/ndk/include_ndk',
'frameworks/native/libs/binder/ndk/include_cpp','system/libbase/include','system/logging/liblog/include',
'system/core/libcutils/include','system/core/libutils/include','system/core/libsystem/include',
'frameworks/native/libs/nativebase/include','frameworks/native/libs/nativewindow/include','frameworks/native/libs/ui/include',
'frameworks/native/libs/math/include','frameworks/native/libs/arect/include','hardware/libhardware/include',
'system/media/camera/include','system/media/private/camera/include','system/media/camera/src',
'frameworks/native/libs/fmqlib/include','system/libfmq/include','system/libhidl/base/include',
'system/libhidl/transport/include','system/libhidl/transport/memory/1.0/default',
'system/libhidl/transport/token/1.0/utils/include','system/libhidl/transport/allocator/1.0/utils/include',
'system/core/libsync/include','system/core/libsystem/include','system/libhidl/libhidlmemory/include',
'hardware/interfaces/graphics/common/aidl/default','hardware/interfaces/graphics/common/aidl',
'external/libyuv/include','external/libjpeg-turbo','external/libexif', 'external/tinyxml2',
'frameworks/av/media/codec2/vndk/include','frameworks/av/media/libstagefright/foundation/include',
'frameworks/native/headers/media_plugin','frameworks/native/opengl/include','frameworks/av/include']
inc += [str(A/p) for p in paths if (A/p).is_dir()]
for base in ['hardware/interfaces/camera','hardware/interfaces/graphics','hardware/interfaces/common','system/hardware/interfaces','system/libhidl/transport','system/libhidl/transport/allocator','system/libhidl/transport/memory','system/libhidl/transport/base','system/libhidl/transport/token']:
    b=O/base
    # Existing generated build headers; these directories contain no source mutation.
    if b.exists():
        for p in b.glob('**/gen'):
            if 'genc++_headers' in str(p) or 'ndk-source' in str(p):
                inc.append(str(p/'include' if (p/'include').is_dir() else p))
# Prefer v2 generated interfaces over v4 for the frozen external implementation.
inc.sort(key=lambda p: ('-V4-' in p, p))
cc=str(N/'toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang++')
flags=['-std=gnu++17','-c','-D__ANDROID_VENDOR__','-DANDROID','-Wall','-Werror','-Wno-unused-parameter','-Wno-missing-field-initializers','-Wno-deprecated-declarations']
flags += ['-I'+str(output/'camera-aidl/include')] + ['-I'+p for p in inc]
for p in (Path('camera/pipewire-provider/src').glob('*.cpp')):
    print('Checking',p,flush=True)
    r=subprocess.run([cc,*flags,str(p),'-o',str(output/(p.stem+'.o'))],capture_output=True,text=True)
    if r.returncode: print(r.stderr);sys.exit(r.returncode)

cc_c=cc.removesuffix('++')
subprocess.run([cc_c, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fPIC', '-c',
    '-I'+str(P/'pipewire-0.3'), '-I'+str(P/'spa-0.2'),
    'camera/pipewire-provider/src/PipeWireCamera.c', '-o', str(output/'PipeWireCamera.o')],check=True)
print('PASS: eight provider/transport objects compiled; final platform link and image remain unverified')
