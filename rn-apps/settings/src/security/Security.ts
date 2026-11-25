import { requireNativeModule } from "expo-modules-core";
import { MatonOS } from "../bridge/MatonOS";

export type SecurityStatus = {
  selinux?: string;
  encryption?: string;
  securityPatch: string;
  vendorSecurityPatch?: string;
  verifiedBoot?: string;
  buildKeys: string;
  adb: string;
};

type NativeSecurity = {
  getSecurityStatus(): Promise<SecurityStatus>;
};

const NativeHardware = requireNativeModule<NativeSecurity>("MatonHardware");

// The app is not platform-signed: SELinux, encryption, vendor patch level and verified boot
// come from the System Bridge; null there means the property is unset.
type BridgeSecurity = Partial<Record<keyof SecurityStatus, string | null>>;

async function getStatus(): Promise<SecurityStatus> {
  const local = await NativeHardware.getSecurityStatus();
  const result = await MatonOS.call("hardware", "security");
  if (!result.available) {
    console.error(`security status unavailable from bridge: ${result.reason}`);
    return local;
  }
  const bridge = JSON.parse(result.value) as BridgeSecurity;
  const merged = { ...local };
  for (const key of Object.keys(bridge) as Array<keyof SecurityStatus>) {
    const value = bridge[key];
    if (typeof value === "string") merged[key] = value;
  }
  return merged;
}

export const Security = { getStatus };
