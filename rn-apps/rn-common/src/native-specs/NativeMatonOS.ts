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
export type RecentTask = {
  taskId: number;
  packageName: string;
  label?: string;
  component?: string;
  windowingMode?: number;
  iconUri?: string;
  thumbnailUri?: string;
};
export type NavigationAction = 'back' | 'home' | 'recents';

export interface Spec extends TurboModule {
  checkStartup(accessTargets: Array<string>, requiredChannels: Array<string>): Promise<BridgeSnapshot>;
  getBridgeSnapshot(accessTarget: string): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  getWallpaperSeedColor(): Promise<number>;
  navigate(action: NavigationAction, longPress: boolean): Promise<CallResult>;
  getRecentTasks(maxTasks: number): Promise<CallResult>;
  moveTaskToFront(taskId: number): Promise<CallResult>;
  setTaskFullscreen(taskId: number): Promise<CallResult>;
  removeRecentTask(taskId: number): Promise<CallResult>;
  getRecentTaskThumbnail(taskId: number): Promise<CallResult>;
  subscribe(target: string, topic: string, subscriptionId: string): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
  addListener(eventName: string): void;
  removeListeners(count: number): void;
}

export default TurboModuleRegistry.getEnforcing<Spec>('MatonOSClient');
