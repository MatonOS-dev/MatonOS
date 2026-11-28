import {requireNativeModule, type EventSubscription} from 'expo-modules-core';
import {MatonOS} from '../bridge/MatonOS';

/** What hardware is REALLY present (MatonOS spoofs missing Wi-Fi/Bluetooth). */
export type HardwareState = {
  battery: {present: boolean; level: number; charging: boolean; pluggedIn: boolean};
  wifi: {present: boolean | null; virtual: boolean | null};
  bluetooth: {present: boolean | null; virtual: boolean | null};
  audio: {present: boolean | null};
  gpu: {present: boolean | null; software: boolean | null};
  camera: {count: number};
};

type HardwareModule = {
  getHardwareState(): Promise<HardwareState>;
  getCameras(): Promise<Array<{id: string; facing: string; jpeg: string; yuv: string}>>;
  addListener(name: 'hardwareChanged', listener: (state: HardwareState) => void): EventSubscription;
};
const NativeHardware = requireNativeModule<HardwareModule>('MatonHardware');

type BridgeSummary = Pick<HardwareState, 'wifi' | 'bluetooth' | 'audio' | 'gpu'>;

/** Daemon-detected hardware comes from the System Bridge; unavailable stays null (unknown). */
async function withBridgeSummary(state: HardwareState): Promise<HardwareState> {
  const result = await MatonOS.call('hardware', 'summary');
  if (!result.available) {
    console.warn(`hardware summary unavailable: ${result.reason}`);
    return state;
  }
  const summary = JSON.parse(result.value) as BridgeSummary;
  return {...state, ...summary};
}

export const Hardware = {
  getState: async (): Promise<HardwareState> =>
    withBridgeSummary(await NativeHardware.getHardwareState()),
  getCameras: (): Promise<Array<{id: string; facing: string; jpeg: string; yuv: string}>> =>
    NativeHardware.getCameras(),
  /** Battery/charging changes, Wi-Fi/Bluetooth state changes. */
  onChange: (listener: (state: HardwareState) => void): EventSubscription =>
    NativeHardware.addListener('hardwareChanged', state => {
      withBridgeSummary(state).then(listener, error => console.error('hardware summary failed', error));
    }),
};
