import { requireNativeModule, type EventSubscription } from "expo-modules-core";

export type BridgeSnapshot = {
  available: boolean;
  reason: string;
  apiVersion: number;
  accessAllowed: boolean;
  missingChannels: string[];
};
export type CallResult = { available: boolean; value: string; reason: string };
type EventMap = {
  matonosAreaEvent: (event: MatonAreaEvent) => void;
  matonosAvailabilityChanged: (event: BridgeSnapshot) => void;
};
type NativeEvents<E> = {
  addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription;
};
type MatonOSClientModule = NativeEvents<EventMap> & {
  checkStartup(
    accessTargets: string[],
    requiredChannels: string[],
  ): Promise<BridgeSnapshot>;
  getBridgeSnapshot(accessTarget: string): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  stageUpdate(names: string[], uris: string[]): Promise<CallResult>;
  finalizeUpdate(): Promise<CallResult>;
  getUpdateStatus(): Promise<CallResult>;
  isLiveImage(): Promise<boolean>;
  pickAndInstallGapps(): Promise<{ cancelled?: boolean; ok?: boolean; message: string; installed?: string[] }>;
  uninstallGappsUpdates(): Promise<{ ok: boolean; message: string }>;
  getGappsDeviceId(): Promise<string>;
  getSystemInfo(): Promise<{
    version: string;
    build: string;
    device: string;
    model: string;
    sdk: number;
  }>;
  isDeveloperOptionsEnabled(): Promise<boolean>;
  enableDeveloperOptions(): Promise<boolean>;
  accountMachineInfo(): Promise<{ machineId: string; deviceName: string }>;
  accountComplete(username: string, email: string, token: string, refreshToken: string, machineId: string): Promise<boolean>;
  accountInvalidate(token: string): Promise<boolean>;
  accountStoredSession(): Promise<{ username:string; email:string; token:string; refreshToken:string } | null>;
  accountRemoveLocal(username: string): Promise<boolean>;
  accountCancel(): Promise<boolean>;
  openTrustedApps(): Promise<boolean>;
  finishActivity(): Promise<boolean>;
  reboot(): Promise<boolean>;
  subscribe(
    target: string,
    topic: string,
    subscriptionId: string,
  ): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
};
const Native = requireNativeModule<MatonOSClientModule>("MatonOSClient");
let nextSubscription = 1;
export type MatonAreaEvent = { target: string; topic: string; json: string };
export const MatonOS = {
  checkStartup: (targets: string[], channels: string[] = []) =>
    Native.checkStartup(targets, channels),
  getBridgeSnapshot: (target: string) => Native.getBridgeSnapshot(target),
  call: (
    target: string,
    command: string,
    args: unknown = {},
  ): Promise<CallResult> => Native.call(target, command, JSON.stringify(args)),
  stageUpdate: (names: string[], uris: string[]) => Native.stageUpdate(names, uris),
  finalizeUpdate: () => Native.finalizeUpdate(),
  getUpdateStatus: () => Native.getUpdateStatus(),
  isLiveImage: () => Native.isLiveImage(),
  getSystemInfo: () => Native.getSystemInfo(),
  isDeveloperOptionsEnabled: () => Native.isDeveloperOptionsEnabled(),
  enableDeveloperOptions: () => Native.enableDeveloperOptions(),
  pickAndInstallGapps: () => Native.pickAndInstallGapps(),
  uninstallGappsUpdates: () => Native.uninstallGappsUpdates(),
  getGappsDeviceId: () => Native.getGappsDeviceId(),
  accountMachineInfo: () => Native.accountMachineInfo(),
  accountComplete: (username: string, email: string, token: string, refreshToken: string, machineId: string) => Native.accountComplete(username, email, token, refreshToken, machineId),
  accountInvalidate: (token: string) => Native.accountInvalidate(token),
  accountStoredSession: () => Native.accountStoredSession(),
  accountRemoveLocal: (username: string) => Native.accountRemoveLocal(username),
  accountCancel: () => Native.accountCancel(),
  openTrustedApps: () => Native.openTrustedApps(),
  finishActivity: () => Native.finishActivity(),
  reboot: () => Native.reboot(),
  subscribe: (
    target: string,
    topic: string,
    listener: (event: MatonAreaEvent) => void,
  ) => {
    const id = `${target}:${topic}:${nextSubscription++}`;
    const subscription = Native.addListener("matonosAreaEvent", (event) => {
      const value = event as MatonAreaEvent;
      if (value.target === target && value.topic === topic) listener(value);
    });
    void Native.subscribe(target, topic, id);
    return {
      remove: () => {
        subscription.remove();
        void Native.unsubscribe(id);
      },
    };
  },
};
