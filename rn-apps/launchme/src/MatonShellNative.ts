import {requireNativeModule, type EventSubscription} from 'expo-modules-core';
import type {RecentTask} from '@matonos/rn-common';

export type LauncherApp = {packageName: string; component: string; label: string; iconUri: string};
export type {RecentTask};
export type ShellEvent = {type: string; packageName?: string};

type EventMap = {
  packagesChanged: (event: ShellEvent) => void;
  wallpaperChanged: () => void;
};
type NativeEvents<E> = {addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription};
type MatonShellModule = NativeEvents<EventMap> & {
  getLauncherApps(): Promise<LauncherApp[]>;
  launchApp(component: string): Promise<boolean>;
  getWallpaperSeedColor(): Promise<number>;
  togglePinnedApp(packageName: string): Promise<boolean>;
  goHome(): void;
  reportSurfaceFailure(surfaceName: string, error: string): void;
};

const NativeMatonShell = requireNativeModule<MatonShellModule>('MatonShell');

export const MatonShell = {
  getLauncherApps: (): Promise<LauncherApp[]> => NativeMatonShell.getLauncherApps(),
  launchApp: (component: string): Promise<boolean> => NativeMatonShell.launchApp(component),
  getWallpaperSeedColor: () => NativeMatonShell.getWallpaperSeedColor(),
  togglePinnedApp: (packageName: string) => NativeMatonShell.togglePinnedApp(packageName),
  goHome: () => NativeMatonShell.goHome(),
  reportSurfaceFailure: (surfaceName: string, error: string) =>
    NativeMatonShell.reportSurfaceFailure(surfaceName, error),
  onPackagesChanged: (listener: (event: ShellEvent) => void) =>
    NativeMatonShell.addListener('packagesChanged', listener),
  onWallpaperChanged: (listener: () => void) =>
    NativeMatonShell.addListener('wallpaperChanged', listener),
};
