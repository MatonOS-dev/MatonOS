import { requireNativeModule } from "expo-modules-core";

export type EmbeddedSettingsEntry = {
  component: string;
  label: string;
  package: string;
  icon: string;
};

type NativeEmbeddedSettings = {
  getEmbeddedSettings(): Promise<EmbeddedSettingsEntry[]>;
};

const NativeHardware = requireNativeModule<NativeEmbeddedSettings>("MatonHardware");

export const EmbeddedSettings = {
  list: (): Promise<EmbeddedSettingsEntry[]> => NativeHardware.getEmbeddedSettings(),
};
