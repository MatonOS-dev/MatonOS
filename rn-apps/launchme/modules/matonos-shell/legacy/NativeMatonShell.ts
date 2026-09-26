import type {TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export type LauncherApp = {
  packageName: string;
  component: string;
  label: string;
  iconUri: string;
};

export type RecentTask = {
  taskId: number;
  packageName: string;
  label: string;
  component: string;
  windowingMode: number;
  iconUri: string;
  thumbnailUri: string;
};

export type ShelfState = {
  expanded: boolean;
  homeVisible: boolean;
  panel: string;
};

export interface Spec extends TurboModule {
  getLauncherApps(): Promise<Array<LauncherApp>>;
  launchApp(component: string): Promise<boolean>;
  getRecentTasks(): Promise<Array<RecentTask>>;
  moveTaskToFront(taskId: number): Promise<boolean>;
  setTaskFullscreen(taskId: number): Promise<boolean>;
  closeRecentTask(taskId: number): Promise<boolean>;
  getTaskThumbnail(taskId: number): Promise<string>;
  getWallpaperSeedColor(): Promise<number>;
  getShelfState(): Promise<ShelfState>;
  getPinnedApps(): Promise<Array<string>>;
  togglePinnedApp(packageName: string): Promise<boolean>;
  setShelfExpanded(expanded: boolean): void;
  openPanel(panel: string): void;
  goHome(): void;
  reportSurfaceFailure(surfaceName: string, error: string): void;
  addListener(eventName: string): void;
  removeListeners(count: number): void;
}

export default TurboModuleRegistry.getEnforcing<Spec>('MatonShell');
