import type { Rating, Review, StoreApp } from "./Source";

// GNOME's Open Desktop Ratings Service, the review backend GNOME Software uses for Flathub apps.
const ODRS = "https://odrs.gnome.org/1.0/reviews/api";

type RawRating = { star1: number; star2: number; star3: number; star4: number; star5: number; total: number };
type RawReview = { review_id: number; user_display?: string; rating: number; summary?: string; description?: string; version?: string; date_created: number };

let allRatings: Promise<Record<string, RawRating>> | null = null;

async function getJson<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, init);
  if (!response.ok) throw new Error(`ODRS returned ${response.status} for ${url}.`);
  return response.json() as Promise<T>;
}

function ratingsTable(): Promise<Record<string, RawRating>> {
  allRatings ??= getJson<Record<string, RawRating>>(`${ODRS}/ratings`).catch((error) => {
    allRatings = null; // retry on the next search instead of caching the failure
    throw error;
  });
  return allRatings;
}

function toRating(raw: RawRating | undefined): Rating | undefined {
  if (!raw) return undefined;
  const histogram: Rating["histogram"] = [raw.star1, raw.star2, raw.star3, raw.star4, raw.star5];
  // `total` also counts star0 (no rating), so count only the 1–5 buckets.
  const count = histogram.reduce((acc, votes) => acc + votes, 0);
  if (!count) return undefined;
  const sum = histogram.reduce((acc, votes, index) => acc + votes * (index + 1), 0);
  return { average: Math.round((sum / count) * 10) / 10, count, histogram };
}

// ODRS keys apps by AppStream id; older entries carry a ".desktop" suffix.
function lookup(table: Record<string, RawRating>, id: string): RawRating | undefined {
  return table[id] ?? table[`${id}.desktop`];
}

export async function withRatings(apps: StoreApp[]): Promise<StoreApp[]> {
  const table = await ratingsTable();
  return apps.map((app) => ({ ...app, rating: toRating(lookup(table, app.id)) }));
}

// ODRS requires a client hash; a random one per app session keeps the user unidentifiable.
const userHash = Array.from({ length: 40 }, () => Math.floor(Math.random() * 16).toString(16)).join("");

export async function fetchReviews(app: StoreApp): Promise<Review[]> {
  const raw = await getJson<RawReview[]>(`${ODRS}/fetch`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      app_id: app.id, compat_ids: [`${app.id}.desktop`], user_hash: userHash,
      locale: "en", distro: "MatonOS", version: app.versionName || "unknown", limit: 50,
    }),
  });
  return raw.filter((r) => r.review_id).map((r) => ({
    id: String(r.review_id),
    author: r.user_display || "Anonymous",
    rating: Math.max(1, Math.min(5, Math.round(r.rating / 20))), // ODRS stores 0–100
    summary: r.summary || "",
    description: r.description || "",
    version: r.version || "",
    date: new Date(r.date_created * 1000),
  }));
}
