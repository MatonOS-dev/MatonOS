// Safety summary in the spirit of Flathub's "potentially unsafe" dialog and
// F-Droid's Anti-Features. Sources fill StoreApp.safety / StoreApp.antiFeatures.

export type SafetySeverity = "good" | "neutral" | "warning" | "danger";
export type SafetyItem = { id: string; title: string; description: string; severity: SafetySeverity };
export type SafetyLevel = "safe" | "probably-safe" | "potentially-unsafe";
export type Safety = { level: SafetyLevel; items: SafetyItem[] };
/** F-Droid anti-feature: `reason` is the app-specific note from the repo (may be empty). */
export type AntiFeature = { id: string; title: string; description: string; reason: string };

export function safetyOf(items: SafetyItem[]): Safety {
  const level: SafetyLevel = items.some((item) => item.severity === "danger") ? "potentially-unsafe"
    : items.some((item) => item.severity === "warning") ? "probably-safe" : "safe";
  return { level, items };
}

export type FlatpakPermissions = {
  sockets?: string[] | null;
  devices?: string[] | null;
  shared?: string[] | null;
  filesystems?: string[] | null;
  "session-bus"?: { talk?: string[]; own?: string[] } | null;
  "system-bus"?: { talk?: string[]; own?: string[] } | null;
};

const PRIVILEGED_SOCKETS = ["system-bus", "session-bus", "ssh-auth", "gpg-agent", "pcsc", "cups"];

function filesystemItem(filesystems: string[]): SafetyItem {
  const strip = (fs: string) => fs.replace(/:(rw|ro|create)$/, "");
  const readOnly = (fs: string) => fs.endsWith(":ro");
  const host = filesystems.find((fs) => ["host", "host-os", "host-etc"].includes(strip(fs)));
  if (host) return { id: "fs-host", title: `Full file system ${readOnly(host) ? "read" : "read/write"} access`, description: "Can read" + (readOnly(host) ? "" : " and write") + " all data on the file system", severity: "danger" };
  const home = filesystems.find((fs) => strip(fs) === "home" || strip(fs) === "~");
  if (home) return { id: "fs-home", title: `Home folder ${readOnly(home) ? "read" : "read/write"} access`, description: "Can read" + (readOnly(home) ? "" : " and write") + " all data in your home folder", severity: readOnly(home) ? "warning" : "danger" };
  if (filesystems.length) return { id: "fs-some", title: "User file access", description: `Can access ${filesystems.map(strip).join(", ")}`, severity: "warning" };
  return { id: "fs-none", title: "No file system access", description: "Cannot access the file system at all", severity: "good" };
}

/** Flathub-style rating from flatpak metadata permissions and the licence. */
export function flatpakSafety(permissions: FlatpakPermissions | undefined, freeLicense: boolean | undefined, extraData: boolean): Safety {
  const sockets = permissions?.sockets ?? [];
  const devices = permissions?.devices ?? [];
  const shared = permissions?.shared ?? [];
  const items: SafetyItem[] = [];
  const talksToFlatpak = (permissions?.["session-bus"]?.talk ?? []).some((name) => name.startsWith("org.freedesktop.Flatpak"));
  if (talksToFlatpak) items.push({ id: "sandbox-escape", title: "Can run commands outside its sandbox", description: "Can start any program on the system", severity: "danger" });
  if (sockets.some((socket) => PRIVILEGED_SOCKETS.includes(socket))) items.push({ id: "privileged-socket", title: "Uses a privileged socket", description: "Uses a socket that allows gaining extra permissions", severity: "danger" });
  if (sockets.includes("x11")) items.push({ id: "x11", title: "Legacy windowing system", description: "Uses X11, which lets it see other X11 windows", severity: "warning" });
  items.push(filesystemItem(permissions?.filesystems ?? []));
  items.push(shared.includes("network")
    ? { id: "network", title: "Network access", description: "Has network access", severity: "warning" }
    : { id: "no-network", title: "No network access", description: "Cannot reach the internet", severity: "good" });
  items.push(devices.includes("all")
    ? { id: "devices", title: "User device access", description: "Can access devices such as webcams or gaming controllers", severity: "warning" }
    : { id: "no-devices", title: "No user device access", description: "The app cannot access any user devices such as webcams or gaming controllers", severity: "good" });
  if (extraData) items.push({ id: "extra-data", title: "Downloads from outside Flathub", description: "Part of the app is downloaded from the developer's servers at install time", severity: "warning" });
  items.push(freeLicense
    ? { id: "auditable", title: "Auditable code", description: "The source code is public and can be independently audited, which makes the app more likely to be safe", severity: "good" }
    : { id: "proprietary", title: "Proprietary code", description: "The source code is not public, so it cannot be independently audited", severity: "warning" });
  return safetyOf(items);
}

// Age rating (OARS, as Flathub publishes it in appstream content_rating_details).
export type ContentLevel = "none" | "mild" | "moderate" | "intense";
export type AgeRatingCategory = { id: string; title: string; description: string; level: ContentLevel };
/** minimumAge 0 = all ages; label is the rating system's name for it, e.g. "Teen" (ESRB). */
export type AgeRating = { minimumAge: number; label: string; system: string; categories: AgeRatingCategory[] };

const CATEGORY_TITLES: Record<string, string> = {
  social: "Social", drugs: "Drugs", language: "Strong Language", money: "Money", sex: "Nudity", violence: "Violence",
};

type RawContentRating = {
  categories?: Array<{ id?: string; description?: string; level?: string }>;
  minimumAge?: number; minimumAgeText?: string; contentRatingSystem?: string;
};

export function ageRatingOf(details: Record<string, RawContentRating> | null | undefined): AgeRating | undefined {
  if (!details) return undefined;
  const raw = details.en_US ?? details.C ?? Object.values(details)[0];
  if (!raw || typeof raw.minimumAge !== "number") return undefined;
  const levels: ContentLevel[] = ["none", "mild", "moderate", "intense"];
  return {
    minimumAge: raw.minimumAge,
    label: raw.minimumAgeText ?? "",
    system: raw.contentRatingSystem ?? "",
    categories: (raw.categories ?? []).filter((c) => c.id).map((c) => ({
      id: c.id as string,
      title: CATEGORY_TITLES[c.id as string] ?? (c.id as string),
      description: c.description ?? "",
      level: levels.includes(c.level as ContentLevel) ? (c.level as ContentLevel) : "none",
    })),
  };
}
