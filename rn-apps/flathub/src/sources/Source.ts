import type { AgeRating, AntiFeature, Safety } from "./Safety";
export type { AgeRating, AgeRatingCategory, ContentLevel, AntiFeature, Safety, SafetyItem, SafetyLevel, SafetySeverity } from "./Safety";

export type SourceName = "Flathub" | "F-Droid";

/** What search(), installed() and updates() return: just enough for a list row. */
export type AppListing = {
  id: string;
  source: SourceName;
  name: string;
  summary: string;
  icon: string;
  developer: string;
  /** Average 1–5 and vote count; undefined when the source has no ratings. */
  rating?: Rating;
  installed?: boolean;
  /** Latest version the source offers. */
  versionName?: string;
  /** Repository the listing came from (F-Droid repo URL, Flatpak remote). */
  origin?: string;
};

/** What details() returns: everything known about one app. */
export type AppDetails = AppListing & {
  description: string;
  screenshots: string[];
  permissions: string[];
  license?: string;
  homepage?: string;
  /** Bytes; undefined when the source does not say. */
  downloadSize?: number;
  installedSize?: number;
  /** What the app can do and how risky that is. */
  safety?: Safety;
  /** F-Droid anti-features; [] when the repo lists none, undefined for other sources. */
  antiFeatures?: AntiFeature[];
  /** Flathub only (OARS); undefined when the app declares none. */
  ageRating?: AgeRating;
};

/** Source-internal app record: details plus what install needs. Not part of the hook API. */
export type StoreApp = AppDetails & {
  ref: string;
  versionCode?: number;
  apkName?: string;
  apkSha256?: string;
  apkFiles?: Array<{ name: string; sha256: string }>;
  repository?: string;
};

export function toListing(app: StoreApp): AppListing {
  return { id: app.id, source: app.source, name: app.name, summary: app.summary, icon: app.icon, developer: app.developer,
    rating: app.rating, installed: app.installed, versionName: app.versionName, origin: app.origin ?? app.repository };
}

export function toDetails(app: StoreApp): AppDetails {
  return { ...toListing(app), description: app.description, screenshots: app.screenshots, permissions: app.permissions,
    license: app.license, homepage: app.homepage, downloadSize: app.downloadSize, installedSize: app.installedSize,
    safety: app.safety, antiFeatures: app.antiFeatures, ageRating: app.ageRating };
}

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
