import {requireNativeModule, type EventSubscription} from 'expo-modules-core';

export type BridgeSnapshot = {
  available: boolean;
  reason: string;
  apiVersion: number;
  accessAllowed: boolean;
  missingChannels: string[];
};
export type CallResult = {available: boolean; value: string; reason: string};

type EventMap = {
  matonosAreaEvent: (event: MatonAreaEvent) => void;
  matonosAvailabilityChanged: (event: BridgeSnapshot) => void;
};
type NativeEvents<E> = {addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription};
type MatonOSClientModule = NativeEvents<EventMap> & {
  checkStartup(accessTargets: string[], requiredChannels: string[]): Promise<BridgeSnapshot>;
  getBridgeSnapshot(accessTarget: string): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  subscribe(target: string, topic: string, subscriptionId: string): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
  injectBackKey(): Promise<CallResult>;
};

export type MatonAreaEvent = {
  target: string;
  topic: string;
  json: string;
};

const NativeMatonOS = requireNativeModule<MatonOSClientModule>('MatonOSClient');
const emitter = NativeMatonOS;
let nextSubscription = 1;

export const MatonOS = {
  checkStartup: (accessTargets: string[], requiredChannels: string[] = []) =>
    NativeMatonOS.checkStartup(accessTargets, requiredChannels),

  getBridgeSnapshot: (accessTarget: string) =>
    NativeMatonOS.getBridgeSnapshot(accessTarget),

  call: (target: string, command: string, args: unknown = {}): Promise<CallResult> =>
    NativeMatonOS.call(target, command, JSON.stringify(args)),

  injectBackKey: () => NativeMatonOS.injectBackKey(),

  subscribe: (
    target: string,
    topic: string,
    listener: (event: MatonAreaEvent) => void,
  ) => {
    const subscriptionId = `${target}:${topic}:${nextSubscription++}`;
    const subscription = emitter.addListener('matonosAreaEvent', event => {
      const areaEvent = event as MatonAreaEvent;
      if (areaEvent.target === target && areaEvent.topic === topic) listener(areaEvent);
    });
    void NativeMatonOS.subscribe(target, topic, subscriptionId);
    return {
      remove: () => {
        subscription.remove();
        void NativeMatonOS.unsubscribe(subscriptionId);
      },
    };
  },

  onAvailabilityChanged: (listener: (snapshot: BridgeSnapshot) => void) =>
    emitter.addListener('matonosAvailabilityChanged', event => listener(event as BridgeSnapshot)),
};
