import {requireNativeModule, type EventSubscription} from 'expo-modules-core';

export type LauncherApp = {packageName: string; component: string; label: string; iconUri: string};
export type ShelfState = {
  expanded: boolean;
  homeVisible: boolean;
  panel: string;
  threeButtonMode: boolean;
};
export type ShelfEvent = {
  type: string;
  packageName?: string;
  expanded?: boolean;
  homeVisible?: boolean;
  panel?: string;
};

type EventMap = {
  shelfStateChanged: (event: ShelfEvent) => void;
  packagesChanged: (event: ShelfEvent) => void;
};
type NativeEvents<E> = {addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription};
type MatonShelfModule = NativeEvents<EventMap> & {
  getLauncherApps(): Promise<LauncherApp[]>;
  launchApp(component: string): Promise<boolean>;
  getWallpaperSeedColor(): Promise<number>;
  getShelfState(): Promise<ShelfState>;
  getPinnedApps(): Promise<string[]>;
  togglePinnedApp(packageName: string): Promise<boolean>;
  setShelfExpanded(expanded: boolean): void;
  setThreeButtonMode(enabled: boolean): void;
  openPanel(panel: 'drawer' | 'recents'): void;
  goHome(): void;
  reportSurfaceFailure(error: string): void;
};

const NativeMatonShelf = requireNativeModule<MatonShelfModule>('MatonShelf');

export const MatonShelf = {
  getLauncherApps: () => NativeMatonShelf.getLauncherApps(),
  launchApp: (component: string) => NativeMatonShelf.launchApp(component),
  getWallpaperSeedColor: () => NativeMatonShelf.getWallpaperSeedColor(),
  getShelfState: () => NativeMatonShelf.getShelfState(),
  getPinnedApps: () => NativeMatonShelf.getPinnedApps(),
  togglePinnedApp: (packageName: string) => NativeMatonShelf.togglePinnedApp(packageName),
  setShelfExpanded: (expanded: boolean) => NativeMatonShelf.setShelfExpanded(expanded),
  setThreeButtonMode: (enabled: boolean) => NativeMatonShelf.setThreeButtonMode(enabled),
  openPanel: (panel: 'drawer' | 'recents') => NativeMatonShelf.openPanel(panel),
  goHome: () => NativeMatonShelf.goHome(),
  reportSurfaceFailure: (error: string) => NativeMatonShelf.reportSurfaceFailure(error),
  onShelfState: (listener: (state: ShelfEvent) => void) =>
    NativeMatonShelf.addListener('shelfStateChanged', listener),
  onPackagesChanged: (listener: (event: ShelfEvent) => void) =>
    NativeMatonShelf.addListener('packagesChanged', listener),
};
