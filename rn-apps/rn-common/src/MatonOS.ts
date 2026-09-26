import {requireNativeModule, type EventSubscription} from 'expo-modules-core';

export type BridgeSnapshot = {
  available: boolean;
  reason: string;
  apiVersion: number;
  accessAllowed: boolean;
  missingChannels: string[];
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

type EventMap = {
  matonosAreaEvent: (event: MatonAreaEvent) => void;
  matonosAvailabilityChanged: (event: BridgeSnapshot) => void;
};
type NativeEvents<E> = {addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription};
type MatonOSClientModule = NativeEvents<EventMap> & {
  checkStartup(accessTargets: string[], requiredChannels: string[]): Promise<BridgeSnapshot>;
  getBridgeSnapshot(accessTarget: string): Promise<BridgeSnapshot>;
  call(target: string, command: string, jsonArgs: string): Promise<CallResult>;
  getWallpaperSeedColor(): Promise<number>;
  subscribe(target: string, topic: string, subscriptionId: string): Promise<boolean>;
  unsubscribe(subscriptionId: string): Promise<boolean>;
  navigate(action: NavigationAction, longPress: boolean): Promise<CallResult>;
  getRecentTasks(maxTasks: number): Promise<CallResult>;
  moveTaskToFront(taskId: number): Promise<CallResult>;
  setTaskFullscreen(taskId: number): Promise<CallResult>;
  removeRecentTask(taskId: number): Promise<CallResult>;
  getRecentTaskThumbnail(taskId: number): Promise<CallResult>;
};

export type NavigationAction = 'back' | 'home' | 'recents';

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
  getWallpaperSeedColor: () => NativeMatonOS.getWallpaperSeedColor(),

  navigate: async (action: NavigationAction, longPress = false): Promise<boolean> => {
    const result = await NativeMatonOS.navigate(action, longPress);
    return result.available && result.value === 'true';
  },

  getRecentTasks: async (maxTasks = 32): Promise<RecentTask[]> => {
    const result = await NativeMatonOS.getRecentTasks(maxTasks);
    if (!result.available || !result.value) return [];
    try {
      return JSON.parse(result.value) as RecentTask[];
    } catch {
      return [];
    }
  },

  moveTaskToFront: async (taskId: number): Promise<boolean> => {
    const result = await NativeMatonOS.moveTaskToFront(taskId);
    return result.available && result.value === 'true';
  },

  setTaskFullscreen: async (taskId: number): Promise<boolean> => {
    const result = await NativeMatonOS.setTaskFullscreen(taskId);
    return result.available && result.value === 'true';
  },

  removeRecentTask: async (taskId: number): Promise<boolean> => {
    const result = await NativeMatonOS.removeRecentTask(taskId);
    return result.available && result.value === 'true';
  },

  getRecentTaskThumbnail: async (taskId: number): Promise<string> => {
    const result = await NativeMatonOS.getRecentTaskThumbnail(taskId);
    return result.available ? result.value : '';
  },

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
