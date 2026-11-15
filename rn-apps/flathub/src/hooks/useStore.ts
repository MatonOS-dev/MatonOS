import { useEffect, useState, useSyncExternalStore } from "react";
import { pluginSources } from "../sources/PluginSource";
import { MatonOS } from "../../modules/matonos-flathub/src/MatonOS";
import { FlathubSource } from "../sources/FlathubSource";
import { toDetails, toListing, type AppDetails, type AppListing, type Review, type Source, type SourceName, type StoreApp } from "../sources/Source";

export type * from "../sources/Source";

let SOURCES: Source[] = [FlathubSource];
let sourceLoad: Promise<void> | null = null;
const sourceListeners = new Set<() => void>();
let progressSubscriptions: Array<{ remove(): void }> = [];
export function loadSources(): Promise<void> {
  sourceLoad ??= pluginSources().then((plugins) => {
    progressSubscriptions.forEach((subscription) => subscription.remove());
    SOURCES = [FlathubSource, ...plugins]; installedIds = null;
    progressSubscriptions = SOURCES.map((source) => source.progress((event) => { if (event.result) installedIds = null; }));
    sourceListeners.forEach((listener) => listener());
  }).catch((error) => { sourceLoad = null; console.error("Loading source plugins failed", error); throw error; });
  return sourceLoad;
}
export async function setPluginEnabled(pkg: string, enabled: boolean): Promise<void> {
  await MatonOS.setSourceEnabled(pkg, enabled); sourceLoad = null; await loadSources();
}
function subscribeSources(listener: () => void) { sourceListeners.add(listener); return () => { sourceListeners.delete(listener); }; }
export function useSources(): Source[] {
  const sources = useSyncExternalStore(subscribeSources, () => SOURCES);
  useEffect(() => { void loadSources().catch((error) => console.error("Source discovery failed", error)); }, []);
  return sources;
}
const SEARCH_DEBOUNCE_MS = 300;

function sourceFor(name: SourceName): Source {
  const source = SOURCES.find((candidate) => candidate.name === name);
  if (!source) throw new Error(`Unknown source ${name}.`);
  return source;
}

// Installed ids per source, cached for the session and dropped whenever an
// install/uninstall finishes so the next search sees the new state.
let installedIds: Promise<Set<string>> | null = null;


async function installed(): Promise<Set<string>> {
  await loadSources();
  installedIds ??= Promise.all(SOURCES.map((source) => source.installed().catch((error) => {
    console.warn(`${source.name}: listing installed apps failed`, error);
    return [] as StoreApp[];
  }))).then((lists) => new Set(lists.flat().map((app) => `${app.source}:${app.id}`)));
  return installedIds;
}

async function searchSource(source: Source, term: string, ids: Set<string>): Promise<AppListing[]> {
  const found = await source.search(term);
  const rated = source.ratings ? await source.ratings(found).catch((error) => {
    console.warn(`${source.name}: ratings unavailable`, error);
    return found;
  }) : found;
  return rated.map((app) => toListing({ ...app, installed: ids.has(`${app.source}:${app.id}`) }));
}

export type SearchState = {
  apps: AppListing[];
  loading: boolean;
  /** One entry per source that failed; results from the other sources still show. */
  errors: Array<{ source: SourceName; message: string }>;
};

/** Searches every source (debounced). An empty term returns no apps. */
export function useAppSearch(term: string): SearchState {
  const sources = useSources();
  const query = term.trim();
  const [result, setResult] = useState<{ query: string; apps: AppListing[]; errors: SearchState["errors"] }>({ query: "", apps: [], errors: [] });

  useEffect(() => {
    if (!query) return;
    let stale = false;
    const timer = setTimeout(async () => {
      const ids = await installed();
      const results = await Promise.allSettled(SOURCES.map((source) => searchSource(source, query, ids)));
      if (stale) return;
      const apps: AppListing[] = [];
      const errors: SearchState["errors"] = [];
      results.forEach((outcome, index) => {
        if (outcome.status === "fulfilled") apps.push(...outcome.value);
        else {
          console.error(`${SOURCES[index].name}: search for "${query}" failed`, outcome.reason);
          errors.push({ source: SOURCES[index].name, message: String(outcome.reason?.message ?? outcome.reason) });
        }
      });
      setResult({ query, apps, errors });
    }, SEARCH_DEBOUNCE_MS);
    return () => { stale = true; clearTimeout(timer); };
  }, [query, sources]);

  if (!query) return { apps: [], loading: false, errors: [] };
  // Keep the previous results on screen while the new search runs.
  return { apps: result.apps, loading: result.query !== query, errors: result.errors };
}

export type DetailsState = {
  /** Full details once loaded; null while loading or without an app. */
  app: AppDetails | null;
  /** undefined: the source has no reviews backend. [] : supported but none. */
  reviews: Review[] | undefined;
  loading: boolean;
  error: string | null;
};

/** Full details (description, screenshots, permissions, sizes) and reviews for one app. */
export function useAppDetails(app: AppListing | null): DetailsState {
  const key = app ? `${app.source}:${app.id}` : "";
  const [result, setResult] = useState<{ key: string; app: AppDetails | null; reviews: Review[] | undefined; error: string | null }>({ key: "", app: null, reviews: undefined, error: null });

  useEffect(() => {
    if (!app) return;
    let stale = false;
    const source = sourceFor(app.source);
    const reviews = source.reviews
      ? source.reviews(app as StoreApp).catch((error) => { console.warn(`${source.name}: reviews for ${app.id} failed`, error); return [] as Review[]; })
      : Promise.resolve(undefined);
    Promise.all([source.details(app as StoreApp), reviews, installed()]).then(([details, list, ids]) => {
      if (!stale) setResult({ key, app: toDetails({ ...details, rating: details.rating ?? app.rating, installed: ids.has(key) }), reviews: list, error: null });
    }).catch((error) => {
      console.error(`${source.name}: details for ${app.id} failed`, error);
      if (!stale) setResult({ key, app: null, reviews: undefined, error: String(error?.message ?? error) });
    });
    return () => { stale = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- keyed by source+id, not object identity
  }, [key]);

  if (!app) return { app: null, reviews: undefined, loading: false, error: null };
  if (result.key !== key) return { app: null, reviews: undefined, loading: true, error: null };
  return { app: result.app, reviews: result.reviews, loading: false, error: result.error };
}
