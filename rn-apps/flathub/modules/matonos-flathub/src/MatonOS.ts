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
  startStubInstall(ref: string, operationId: string | null): Promise<boolean>;
  startStubUpdate(ref: string, operationId: string | null): Promise<boolean>;
  launchStub(appId: string): Promise<boolean>;
  sourcePlugins(): Promise<SourcePlugin[]>;
  setSourceEnabled(pkg: string, enabled: boolean): Promise<void>;
  sourceInfo(pkg: string): Promise<string>;
  sourceSearch(pkg: string, term: string): Promise<string>;
  sourceDetails(pkg: string, id: string): Promise<string>;
  sourceReviews(pkg: string, id: string): Promise<string>;
  sourceInstalled(pkg: string): Promise<string>;
  sourceUpdates(pkg: string): Promise<string>;
  sourceRepos(pkg: string): Promise<string>;
  sourceAddRepo(pkg: string, url: string): Promise<void>;
  sourceDelRepo(pkg: string, id: string): Promise<void>;
  sourceInstall(pkg: string, id: string, operationId: string): Promise<void>;
  sourceUninstall(pkg: string, packageName: string, operationId: string): Promise<void>;
};
/** An installed store backend app. Preinstalled ones start enabled, others off. */
export type SourcePlugin = { package: string; service: string; label: string; preinstalled: boolean; enabled: boolean };
export type StoreEvent = { source?: string; operationId: string; phase: string; percent?: number; bytesDone?: number; bytesTotal?: number; result?: { ok?: boolean; error?: string; output?: string; operationId?: string } };
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
  sourcePlugins: () => Native.sourcePlugins(),
  setSourceEnabled: (pkg: string, enabled: boolean) => Native.setSourceEnabled(pkg, enabled),
  sourceInfo: (pkg: string) => Native.sourceInfo(pkg),
  sourceSearch: (pkg: string, term: string) => Native.sourceSearch(pkg, term),
  sourceDetails: (pkg: string, id: string) => Native.sourceDetails(pkg, id),
  sourceReviews: (pkg: string, id: string) => Native.sourceReviews(pkg, id),
  sourceInstalled: (pkg: string) => Native.sourceInstalled(pkg),
  sourceUpdates: (pkg: string) => Native.sourceUpdates(pkg),
  sourceRepos: (pkg: string) => Native.sourceRepos(pkg),
  sourceAddRepo: (pkg: string, url: string) => Native.sourceAddRepo(pkg, url),
  sourceDelRepo: (pkg: string, id: string) => Native.sourceDelRepo(pkg, id),
  sourceInstall: (pkg: string, id: string, operationId: string) => Native.sourceInstall(pkg, id, operationId),
  sourceUninstall: (pkg: string, packageName: string, operationId: string) => Native.sourceUninstall(pkg, packageName, operationId),
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
