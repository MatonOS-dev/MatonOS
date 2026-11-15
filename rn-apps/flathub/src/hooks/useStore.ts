import { useEffect, useState } from "react";
import { FdroidSource } from "../sources/FdroidSource";
import { FlathubSource } from "../sources/FlathubSource";
import type { Review, Source, SourceName, StoreApp } from "../sources/Source";

export type { Rating, Review, SourceName, StoreApp } from "../sources/Source";

const SOURCES: Source[] = [FlathubSource, FdroidSource];
const SEARCH_DEBOUNCE_MS = 300;

function sourceFor(name: SourceName): Source {
  const source = SOURCES.find((candidate) => candidate.name === name);
  if (!source) throw new Error(`Unknown source ${name}.`);
  return source;
}

// Installed ids per source, cached for the session and dropped whenever an
// install/uninstall finishes so the next search sees the new state.
let installedIds: Promise<Set<string>> | null = null;
SOURCES.forEach((source) => source.progress((event) => { if (event.result) installedIds = null; }));

function installed(): Promise<Set<string>> {
  installedIds ??= Promise.all(SOURCES.map((source) => source.installed().catch((error) => {
    console.warn(`${source.name}: listing installed apps failed`, error);
    return [] as StoreApp[];
  }))).then((lists) => new Set(lists.flat().map((app) => `${app.source}:${app.id}`)));
  return installedIds;
}

async function searchSource(source: Source, term: string, ids: Set<string>): Promise<StoreApp[]> {
  const found = await source.search(term);
  const rated = source.ratings ? await source.ratings(found).catch((error) => {
    console.warn(`${source.name}: ratings unavailable`, error);
    return found;
  }) : found;
  return rated.map((app) => ({ ...app, installed: ids.has(`${app.source}:${app.id}`) }));
}

export type SearchState = {
  apps: StoreApp[];
  loading: boolean;
  /** One entry per source that failed; results from the other sources still show. */
  errors: Array<{ source: SourceName; message: string }>;
};

/** Searches every source (debounced). An empty term returns no apps. */
export function useAppSearch(term: string): SearchState {
  const query = term.trim();
  const [result, setResult] = useState<{ query: string; apps: StoreApp[]; errors: SearchState["errors"] }>({ query: "", apps: [], errors: [] });

  useEffect(() => {
    if (!query) return;
    let stale = false;
    const timer = setTimeout(async () => {
      const ids = await installed();
      const results = await Promise.allSettled(SOURCES.map((source) => searchSource(source, query, ids)));
      if (stale) return;
      const apps: StoreApp[] = [];
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
  }, [query]);

  if (!query) return { apps: [], loading: false, errors: [] };
  // Keep the previous results on screen while the new search runs.
  return { apps: result.apps, loading: result.query !== query, errors: result.errors };
}

export type DetailsState = {
  /** The search result until full details arrive, then the detailed app. */
  app: StoreApp | null;
  /** undefined: the source has no reviews backend. [] : supported but none. */
  reviews: Review[] | undefined;
  loading: boolean;
  error: string | null;
};

/** Full details (description, screenshots, permissions, sizes) and reviews for one app. */
export function useAppDetails(app: StoreApp | null): DetailsState {
  const key = app ? `${app.source}:${app.id}` : "";
  const [result, setResult] = useState<{ key: string; app: StoreApp | null; reviews: Review[] | undefined; error: string | null }>({ key: "", app: null, reviews: undefined, error: null });

  useEffect(() => {
    if (!app) return;
    let stale = false;
    const source = sourceFor(app.source);
    const reviews = source.reviews
      ? source.reviews(app).catch((error) => { console.warn(`${source.name}: reviews for ${app.id} failed`, error); return [] as Review[]; })
      : Promise.resolve(undefined);
    Promise.all([source.details(app), reviews, installed()]).then(([details, list, ids]) => {
      if (!stale) setResult({ key, app: { ...details, rating: details.rating ?? app.rating, installed: ids.has(key) }, reviews: list, error: null });
    }).catch((error) => {
      console.error(`${source.name}: details for ${app.id} failed`, error);
      if (!stale) setResult({ key, app, reviews: undefined, error: String(error?.message ?? error) });
    });
    return () => { stale = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- keyed by source+id, not object identity
  }, [key]);

  if (!app) return { app: null, reviews: undefined, loading: false, error: null };
  if (result.key !== key) return { app, reviews: undefined, loading: true, error: null };
  return { app: result.app, reviews: result.reviews, loading: false, error: result.error };
}
