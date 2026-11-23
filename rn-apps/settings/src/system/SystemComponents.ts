import { requireNativeModule } from "expo-modules-core";

export type SystemComponentCandidate = {
  component: string;
  label: string;
  package: string;
};
export type ProviderChoices = {
  current: string;
  candidates: SystemComponentCandidate[];
};
export type SystemComponentsState = {
  captions: ProviderChoices;
  tray: ProviderChoices;
};

type NativeSystemComponents = {
  getSystemComponents(): Promise<SystemComponentsState>;
  setSystemComponent(kind: "captions" | "tray", flattened: string): Promise<boolean>;
};
const NativeHardware = requireNativeModule<NativeSystemComponents>("MatonHardware");

export const SystemComponents = {
  read: (): Promise<SystemComponentsState> => NativeHardware.getSystemComponents(),
  select: (kind: "captions" | "tray", component: string): Promise<boolean> =>
    NativeHardware.setSystemComponent(kind, component),
};
