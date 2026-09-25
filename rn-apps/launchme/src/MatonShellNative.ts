import {requireNativeModule, type EventSubscription} from 'expo-modules-core';

export type LauncherApp = {packageName: string; component: string; label: string; iconUri: string};
export type RecentTask = {
  taskId: number;
  packageName: string;
  label: string;
  component: string;
  windowingMode: number;
  iconUri: string;
  thumbnailUri: string;
};
export type ShelfState = {expanded: boolean; homeVisible: boolean; panel: string};

export type ShellEvent = {
  type: string;
  packageName?: string;
  expanded?: boolean;
  homeVisible?: boolean;
  panel?: string;
};

type EventMap = {
  shelfStateChanged: (event: ShellEvent) => void;
  packagesChanged: (event: ShellEvent) => void;
};
type NativeEvents<E> = {addListener<K extends keyof E>(name: K, listener: E[K]): EventSubscription};
type MatonShellModule = NativeEvents<EventMap> & {
  getLauncherApps(): Promise<LauncherApp[]>;
  launchApp(component: string): Promise<boolean>;
  getRecentTasks(): Promise<RecentTask[]>;
  moveTaskToFront(taskId: number): Promise<boolean>;
  setTaskFullscreen(taskId: number): Promise<boolean>;
  closeRecentTask(taskId: number): Promise<boolean>;
  getTaskThumbnail(taskId: number): Promise<string>;
  getWallpaperSeedColor(): Promise<number>;
  getShelfState(): Promise<ShelfState>;
  getPinnedApps(): Promise<string[]>;
  togglePinnedApp(packageName: string): Promise<boolean>;
  setShelfExpanded(expanded: boolean): void;
  openPanel(panel: string): void;
  goHome(): void;
  injectBackKey(): Promise<boolean>;
  reportSurfaceFailure(surfaceName: string, error: string): void;
};

const NativeMatonShell = requireNativeModule<MatonShellModule>('MatonShell');
const emitter = NativeMatonShell;

export const ShellNative = {
  getLauncherApps: (): Promise<LauncherApp[]> => NativeMatonShell.getLauncherApps(),
  launchApp: (component: string): Promise<boolean> => NativeMatonShell.launchApp(component),
  getRecentTasks: (): Promise<RecentTask[]> => NativeMatonShell.getRecentTasks(),
  moveTaskToFront: (taskId: number) => NativeMatonShell.moveTaskToFront(taskId),
  setTaskFullscreen: (taskId: number) => NativeMatonShell.setTaskFullscreen(taskId),
  closeRecentTask: (taskId: number) => NativeMatonShell.closeRecentTask(taskId),
  getTaskThumbnail: (taskId: number) => NativeMatonShell.getTaskThumbnail(taskId),
  getWallpaperSeedColor: () => NativeMatonShell.getWallpaperSeedColor(),
  getShelfState: () => NativeMatonShell.getShelfState(),
  getPinnedApps: () => NativeMatonShell.getPinnedApps(),
  togglePinnedApp: (packageName: string) => NativeMatonShell.togglePinnedApp(packageName),
  setShelfExpanded: (expanded: boolean) => NativeMatonShell.setShelfExpanded(expanded),
  openPanel: (panel: 'drawer' | 'recents') => NativeMatonShell.openPanel(panel),
  goHome: () => NativeMatonShell.goHome(),
  injectBackKey: () => NativeMatonShell.injectBackKey(),
  reportSurfaceFailure: (surfaceName: string, error: string) =>
    NativeMatonShell.reportSurfaceFailure(surfaceName, error),
  onShelfState: (listener: (state: ShellEvent) => void) =>
    emitter.addListener('shelfStateChanged', (event) => listener(event as ShellEvent)),
  onPackagesChanged: (listener: (event: ShellEvent) => void) =>
    emitter.addListener('packagesChanged', (event) => listener(event as ShellEvent)),
};

export const MatonShell = ShellNative;
