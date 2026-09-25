import type {TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export type BridgeSnapshot = {
  available: boolean;
  reason: string;
  apiVersion: number;
  accessAllowed: boolean;
  missingChannels: Array<string>;
};
export type CallResult = {available: boolean; value: string; reason: string};

export interface Spec extends TurboModule {
  checkStartup(accessTargets: Array<string>, requiredChannels: Array<string>): Promise<BridgeSnapshot>;
  getBridgeSnapshot(accessTarget: string): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  subscribe(target: string, topic: string, subscriptionId: string): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
  injectBackKey(): Promise<CallResult>;
  addListener(eventName: string): void;
  removeListeners(count: number): void;
}

export default TurboModuleRegistry.getEnforcing<Spec>('MatonOSClient');
