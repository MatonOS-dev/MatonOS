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
  addListener(name: "linuxAppStoreEvent", listener: (event: StoreEvent) => void): EventSubscription;
  storeInstall(ref: string, operationId: string): Promise<boolean>;
  storeUpdate(ref: string, operationId: string): Promise<boolean>;
  storeUninstall(ref: string, operationId: string): Promise<boolean>;
  storeListInstalled(): Promise<string>;
  storeListRemotes(): Promise<string>;
  fdroidSearch(repository: string, query: string): Promise<Array<Omit<import("../../../src/sources/Source").StoreApp, "source">>>;
  fdroidDetails(repository: string, packageName: string): Promise<Omit<import("../../../src/sources/Source").StoreApp, "source">>;
  fdroidInstalled(repository: string): Promise<Array<Omit<import("../../../src/sources/Source").StoreApp, "source">>>;
  fdroidUpdates(): Promise<Array<Omit<import("../../../src/sources/Source").StoreApp, "source">>>;
  fdroidInstall(repository: string, packageName: string, operationId: string): Promise<boolean>;
  fdroidUninstall(packageName: string, operationId: string): Promise<boolean>;
  startStubInstall(ref: string, operationId: string | null): Promise<boolean>;
  startStubUpdate(ref: string, operationId: string | null): Promise<boolean>;
  launchStub(appId: string): Promise<boolean>;
};
export type StoreEvent = { source?: "F-Droid"; operationId: string; phase: string; percent?: number; bytesDone?: number; bytesTotal?: number; result?: { ok?: boolean; error?: string; output?: string; operationId?: string } };
const Native = requireNativeModule<NativeApi>("MatonOSClient");
let subscriptionIndex = 1;
export const MatonOS = {
  checkStartup: (targets: string[], channels: string[] = []) => Native.checkStartup(targets, channels),
  startStubInstall: (ref: string, operationId?: string) => Native.startStubInstall(ref, operationId ?? null),
  startStubUpdate: (ref: string, operationId?: string) => Native.startStubUpdate(ref, operationId ?? null),
  launchStub: (appId: string) => Native.launchStub(appId),
  call: (target: string, command: string, args: unknown = {}) => Native.call(target, command, JSON.stringify(args)),
  storeInstall: (ref: string, operationId: string) => Native.storeInstall(ref, operationId),
  storeUpdate: (ref: string, operationId: string) => Native.storeUpdate(ref, operationId),
  storeUninstall: (ref: string, operationId: string) => Native.storeUninstall(ref, operationId),
  storeListInstalled: () => Native.storeListInstalled(),
  storeListRemotes: () => Native.storeListRemotes(),
  fdroidSearch: (repository: string, query: string) => Native.fdroidSearch(repository, query),
  fdroidDetails: (repository: string, packageName: string) => Native.fdroidDetails(repository, packageName),
  fdroidInstalled: (repository: string) => Native.fdroidInstalled(repository),
  fdroidUpdates: () => Native.fdroidUpdates(),
  fdroidInstall: (repository: string, packageName: string, operationId: string) => Native.fdroidInstall(repository, packageName, operationId),
  fdroidUninstall: (packageName: string, operationId: string) => Native.fdroidUninstall(packageName, operationId),
  addStoreListener: (listener: (event: StoreEvent) => void) => Native.addListener("linuxAppStoreEvent", listener),
  subscribe: (target: string, topic: string, listener: (event: MatonAreaEvent) => void) => {
    const id = `${target}:${topic}:${subscriptionIndex++}`;
    const subscription = Native.addListener("matonosAreaEvent", (event) => {
      if (event.target === target && event.topic === topic) listener(event);
    });
    void Native.subscribe(target, topic, id);
    return { remove: () => { subscription.remove(); void Native.unsubscribe(id); } };
  },
};
