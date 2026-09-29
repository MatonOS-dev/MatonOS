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
};

type SearchHit = {
  id?: string;
  app_id?: string;
  name?: string;
  summary?: string;
  icon?: string;
  developer_name?: string;
  bundle?: { value?: string };
};
type AppStream = {
  id?: string;
  name?: string;
  summary?: string;
  description?: string;
  icon?: string;
  developer_name?: string;
  bundle?: { value?: string };
  screenshots?: Array<{ sizes?: Array<{ src?: string; width?: string }> }>;
  metadata?: Record<string, unknown>;
};

const API = "https://flathub.org/api/v2";
const FALLBACK_REF = (id: string) => `app/${id}/x86_64/stable`;

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API}${path}`, init);
  if (!response.ok) throw new Error(`Flathub returned ${response.status}.`);
  return response.json() as Promise<T>;
}

function toApp(hit: SearchHit): StoreApp | null {
  const id = hit.app_id || hit.id;
  if (!id || !hit.name) return null;
  return {
    id,
    name: hit.name,
    summary: hit.summary || "",
    icon: hit.icon || "",
    developer: hit.developer_name || "",
    ref: hit.bundle?.value || FALLBACK_REF(id),
    description: "",
    screenshots: [],
    permissions: [],
  };
}

export async function getCollection(name: "popular" | "recently-added"): Promise<StoreApp[]> {
  const result = await request<{ hits?: SearchHit[] }>(`/collection/${name}`);
  return (result.hits || []).map(toApp).filter((app): app is StoreApp => app !== null);
}

export async function searchApps(query: string): Promise<StoreApp[]> {
  const result = await request<{ hits?: SearchHit[] }>("/search", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ query, filters: [] }),
  });
  return (result.hits || []).map(toApp).filter((app): app is StoreApp => app !== null);
}

function permissionList(metadata: Record<string, unknown> | undefined): string[] {
  const raw = metadata?.permissions;
  if (!raw || typeof raw !== "object") return [];
  const permissions: string[] = [];
  for (const [group, entries] of Object.entries(raw as Record<string, unknown>)) {
    if (Array.isArray(entries)) {
      for (const entry of entries) permissions.push(`${group}: ${String(entry)}`);
    } else if (typeof entries === "string" || typeof entries === "boolean") {
      permissions.push(`${group}: ${String(entries)}`);
    }
  }
  return permissions;
}

export async function getAppDetails(app: StoreApp): Promise<StoreApp> {
  const result = await request<AppStream>(`/appstream/${encodeURIComponent(app.id)}`);
  const screenshots: string[] = [];
  for (const shot of result.screenshots || []) {
    const sizes = shot.sizes || [];
    const best = sizes.find((size) => Number(size.width) >= 624 && Boolean(size.src)) || sizes[0];
    if (best?.src) screenshots.push(best.src);
  }
  return {
    ...app,
    name: result.name || app.name,
    summary: result.summary || app.summary,
    icon: result.icon || app.icon,
    developer: result.developer_name || app.developer,
    ref: result.bundle?.value || app.ref,
    description: result.description || "",
    screenshots,
    permissions: permissionList(result.metadata),
  };
}

export async function getAppById(id: string): Promise<StoreApp> {
  const placeholder: StoreApp = { id, name: id, summary: "", icon: "", developer: "", ref: FALLBACK_REF(id), description: "", screenshots: [], permissions: [] };
  return getAppDetails(placeholder);
}
