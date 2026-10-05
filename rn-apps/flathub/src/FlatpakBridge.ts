import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";

export type ProgressEvent = { line?: string; percent?: number; phase?: string; result?: { ok?: boolean; error?: string; output?: string; operationId?: string } };

function decode(value: string): { ok?: boolean; accepted?: boolean; output?: string; error?: string } {
  try { return JSON.parse(value) as { ok?: boolean; accepted?: boolean; output?: string; error?: string }; }
  catch { throw new Error("The Flatpak service returned an invalid response."); }
}

export async function callFlatpak(command: string, args: unknown = {}) {
  let timer: ReturnType<typeof setTimeout> | undefined;
  let result: Awaited<ReturnType<typeof MatonOS.call>>;
  try {
    result = await Promise.race([
      MatonOS.call("flatpak", command, args),
      new Promise<never>((_, reject) => { timer = setTimeout(() => reject(new Error("The Flatpak service timed out.")), 30000); }),
    ]);
  } finally { if (timer) clearTimeout(timer); }
  if (!result.available) throw new Error(result.reason || "The Flatpak service is unavailable.");
  const response = decode(result.value);
  if (!response.ok) {
    const output = response.output?.trim();
    throw new Error(response.error || output || "The Flatpak operation failed.");
  }
  return response;
}

export async function installApp(ref: string, operationId: string, iconBase64?: string) {
  await callFlatpak("add_flathub");
  const args: Record<string, string> = { ref, operationId };
  if (iconBase64) args.icon = iconBase64;
  return callFlatpak("install", args);
}

/** Fetch a Flathub appstream icon as base64 for the stub generator. */
export async function fetchIconBase64(url: string): Promise<string | undefined> {
  if (!url) return undefined;
  try {
    const response = await fetch(url);
    if (!response.ok) return undefined;
    const blob = await response.blob();
    const dataUrl = await new Promise<string>((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(typeof reader.result === "string" ? reader.result : "");
      reader.onerror = () => reject(reader.error);
      reader.readAsDataURL(blob);
    });
    const comma = dataUrl.indexOf(",");
    return comma >= 0 ? dataUrl.slice(comma + 1) : undefined;
  } catch { return undefined; }
}

export async function uninstallApp(ref: string, operationId: string, deleteData = false) {
  return callFlatpak("uninstall", { ref, operationId, deleteData });
}

export async function runApp(appId: string) {
  return callFlatpak("launch_stub", { appId });
}

export async function getInstalledRefs(): Promise<string[]> {
  const response = await callFlatpak("list_installed");
  const output = response.output || "";
  // Flatpak's ref column omits the app/ kind prefix. linuxd lists only apps.
  return output.split(/\r?\n/).map((line) => {
    const ref = line.trim();
    if (/^app\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(ref)) return ref;
    if (/^[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(ref)) return `app/${ref}`;
    return "";
  }).filter(Boolean);
}
