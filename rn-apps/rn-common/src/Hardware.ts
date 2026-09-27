import {requireNativeModule, type EventSubscription} from 'expo-modules-core';

/** What hardware is REALLY present (MatonOS spoofs missing Wi-Fi/Bluetooth). */
export type HardwareState = {
  battery: {present: boolean; level: number; charging: boolean; pluggedIn: boolean};
  wifi: {present: boolean; virtual: boolean};
  bluetooth: {present: boolean; virtual: boolean};
  audio: {present: boolean};
  gpu: {present: boolean; software: boolean};
  camera: {count: number};
};

type HardwareModule = {
  getHardwareState(): Promise<HardwareState>;
  addListener(name: 'hardwareChanged', listener: (state: HardwareState) => void): EventSubscription;
};
const NativeHardware = requireNativeModule<HardwareModule>('MatonHardware');

export const Hardware = {
  getState: (): Promise<HardwareState> => NativeHardware.getHardwareState(),
  /** Battery/charging changes, Wi-Fi/Bluetooth state changes. */
  onChange: (listener: (state: HardwareState) => void): EventSubscription =>
    NativeHardware.addListener('hardwareChanged', listener),
};
