import { requireNativeModule } from "expo-modules-core";

export type SecurityStatus = {
  selinux: string;
  encryption: string;
  securityPatch: string;
  vendorSecurityPatch: string;
  verifiedBoot: string;
  buildKeys: string;
  adb: string;
};

type NativeSecurity = {
  getSecurityStatus(): Promise<SecurityStatus>;
};

const NativeHardware = requireNativeModule<NativeSecurity>("MatonHardware");

export const Security = {
  getStatus: (): Promise<SecurityStatus> => NativeHardware.getSecurityStatus(),
};
