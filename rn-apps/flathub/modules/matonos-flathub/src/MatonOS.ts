import { requireNativeModule, type EventSubscription } from "expo-modules-core";

export type BridgeSnapshot = { available: boolean; reason: string; apiVersion: number; accessAllowed: boolean; missingChannels: string[] };
export type CallResult = { available: boolean; value: string; reason: string };
export type MatonAreaEvent = { target: string; topic: string; json: string };
type NativeApi = {
  addListener(name: "matonosAreaEvent", listener: (event: MatonAreaEvent) => void): EventSubscription;
  checkStartup(accessTargets: string[], requiredChannels: string[]): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  subscribe(target: string, topic: string, subscriptionId: string): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
};
const Native = requireNativeModule<NativeApi>("MatonOSClient");
let subscriptionIndex = 1;
export const MatonOS = {
  checkStartup: (targets: string[], channels: string[] = []) => Native.checkStartup(targets, channels),
  call: (target: string, command: string, args: unknown = {}) => Native.call(target, command, JSON.stringify(args)),
  subscribe: (target: string, topic: string, listener: (event: MatonAreaEvent) => void) => {
    const id = `${target}:${topic}:${subscriptionIndex++}`;
    const subscription = Native.addListener("matonosAreaEvent", (event) => {
      if (event.target === target && event.topic === topic) listener(event);
    });
    void Native.subscribe(target, topic, id);
    return { remove: () => { subscription.remove(); void Native.unsubscribe(id); } };
  },
};
