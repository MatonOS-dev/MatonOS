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
  isLiveImage(): Promise<boolean>;
  getSystemInfo(): Promise<{
    version: string;
    build: string;
    device: string;
    model: string;
    sdk: number;
  }>;
  openTrustedApps(): Promise<boolean>;
  finishActivity(): Promise<boolean>;
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
  isLiveImage: () => Native.isLiveImage(),
  getSystemInfo: () => Native.getSystemInfo(),
  openTrustedApps: () => Native.openTrustedApps(),
  finishActivity: () => Native.finishActivity(),
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
