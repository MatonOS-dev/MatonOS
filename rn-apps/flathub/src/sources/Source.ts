import type { AntiFeature, Safety } from "./Safety";
export type { AntiFeature, Safety, SafetyItem, SafetyLevel, SafetySeverity } from "./Safety";

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
  repository?: string;
  /** Average 1–5 and vote count; undefined when the source has no ratings. */
  rating?: Rating;
  installed?: boolean;
  license?: string;
  homepage?: string;
  /** Bytes; undefined when the source does not say. */
  downloadSize?: number;
  installedSize?: number;
  /** Filled by details(): what the app can do and how risky that is. */
  safety?: Safety;
  /** F-Droid anti-features; [] when the repo lists none, undefined for other sources. */
  antiFeatures?: AntiFeature[];
};

export type Rating = { average: number; count: number; histogram: [number, number, number, number, number] };

export type Review = {
  id: string;
  author: string;
  rating: number; // 1–5
  summary: string;
  description: string;
  version: string;
  date: Date;
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
  /** Optional: sources without a reviews backend leave it out. */
  reviews?(app: StoreApp): Promise<Review[]>;
  /** Optional: fills app.rating for a batch of results. */
  ratings?(apps: StoreApp[]): Promise<StoreApp[]>;
  progress(listener: (event: SourceProgress) => void): { remove(): void };
}

export function unsupported(operation: string): Error {
  return new Error(`${operation} is not supported for this source yet.`);
}
