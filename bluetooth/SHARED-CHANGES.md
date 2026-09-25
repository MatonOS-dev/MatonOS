# Shared changes requested for Bluetooth NDK integration

The Bluetooth sources now live in `native/matonos-btd/` and
`native/matonos-bluetooth-service/`, built outside Soong. Please make these
buildinfra/tool changes before the next image build:

1. Add fixed `cc_prebuilt_binary` imports in `buildinfra/Android.bp`:

   ```bp
   cc_prebuilt_binary {
       name: "matonos-btd",
       srcs: ["native-built/matonos-btd"],
       vendor: true,
       shared_libs: ["libmatonos-ipc", "liblog"],
   }

   cc_prebuilt_binary {
       name: "matonos-bluetooth-service",
       srcs: ["native-built/matonos-bluetooth-service"],
       stem: "matonos-bluetooth-service",
       relative_install_path: "hw",
       vendor: true,
       shared_libs: ["libbinder_ndk", "liblog"],
   }
   ```

   Their init rc and VINTF files are copied by `bluetooth/bluetooth.mk`.
   Reason: remove Bluetooth source compilation from Soong while retaining
   stable module names in the product package list.

2. In `tools/build-native.sh`, pass `--structured --stability=vintf` for
   `native/matonos-bluetooth-service/aidl/*.aidl`. These files carry
   `@VintfStability`; aidl rejects code generation without these options.

3. The service uses Binder NDK service registration APIs that are platform
   headers, and those symbols are exported by AOSP's platform
   `libbinder_ndk.so`, not the NDK SDK library. The service CMake file links
   the current x86_64 platform artifact at
   `out/soong/.intermediates/frameworks/native/libs/binder/ndk/libbinder_ndk/android_vendor_x86_64_shared/libbinder_ndk.so`.
   Please make the `build-native.sh` order/contract ensure that this platform
   artifact exists before compiling the Bluetooth service. The normal AOSP
   product already includes `libbinder_ndk` at runtime.

4. The daemon socket ACL is consolidated in
   `sepolicy/matonos/matonos_bridge.te`; all daemon sockets share the
   `matonos_socket` label and the driver runs in `matonos_driver`.

   The daemon uses the shared length-prefixed JSON channel at
   `/dev/socket/matonos/bluetooth`. Its commands are `list_devices` (returns
   `{"devices":[{"id":"...","hci":"hci0","index":0,"usb":false}]}`) and
   `select_device` (`{\"id\":\"<listed stable ID>\"}`); only currently
   enumerated IDs can be selected. Reason: keep hardware selection in the
   daemon while exposing the bridge's generic v2 control transport.

The prior Bluetooth SystemConfig gate and Bluetooth stub patches have been
removed/reverted per the zero-patch v1.2 decision. The unavailable-features
XML remains without Bluetooth entries; the Bluetooth feature XML remains
unconditional because VHCI supplies a backend without hardware.

The audio agent consumes `vendor.maton.bluetooth.present`, which is `1` only
for a real selected controller. `vendor.maton.bluetooth.available` means
either a real or virtual controller is ready and must not gate Bluetooth
audio modules.
