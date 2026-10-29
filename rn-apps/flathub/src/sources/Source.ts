export type SourceName = "Flathub" | "F-Droid";

export type StoreApp = {
  id: string;
  name: string;
  summary: string;
  icon: string;
  developer: string;
  ref: string;
  description: string;
  screenshots: string[];
  permissions: string[];
  source: SourceName;
  versionName?: string;
  versionCode?: number;
  apkName?: string;
  apkSha256?: string;
  apkFiles?: Array<{ name: string; sha256: string }>;
};

export type SourceProgress = { source?: SourceName; operationId?: string; phase?: string; percent?: number; bytesDone?: number; bytesTotal?: number; result?: { ok?: boolean; error?: string; output?: string; operationId?: string } };

export interface Source {
  readonly name: SourceName;
  search(query: string): Promise<StoreApp[]>;
  details(app: StoreApp): Promise<StoreApp>;
  installed(): Promise<StoreApp[]>;
  updates(): Promise<StoreApp[]>;
  install(app: StoreApp, operationId: string): Promise<void>;
  uninstall(app: StoreApp, operationId: string): Promise<void>;
  progress(listener: (event: SourceProgress) => void): { remove(): void };
}

export function unsupported(operation: string): Error {
  return new Error(`${operation} is not supported for this source yet.`);
}
