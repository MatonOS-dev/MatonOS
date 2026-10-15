import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";

export type ProgressEvent = { operationId?: string; line?: string; percent?: number; phase?: string; bytesDone?: number; bytesTotal?: number; result?: { ok?: boolean; error?: string; output?: string; operationId?: string } };

function decode(value: string): { ok?: boolean; output?: string; error?: string } {
  try { return JSON.parse(value) as { ok?: boolean; output?: string; error?: string }; }
  catch { throw new Error("The Flatpak service returned an invalid response."); }
}

export async function installApp(ref: string, operationId: string) {
  return MatonOS.storeInstall(ref, operationId);
}

export async function updateApp(ref: string, operationId: string) {
  return MatonOS.storeUpdate(ref, operationId);
}

export async function uninstallApp(ref: string, operationId: string) {
  return MatonOS.storeUninstall(ref, operationId);
}

export async function runApp(appId: string) {
  const response = await MatonOS.launchStub(appId);
  if (!response) throw new Error("The app could not be opened.");
}

function outputFromStoreResponse(raw: string): string {
  const response = decode(raw);
  if (!response.ok) throw new Error(response.error || response.output?.trim() || "The Flatpak operation failed.");
  return response.output || "";
}

/** Inventory is scoped to the calling Android user by LinuxAppStoreService. */
export async function getInstalledRefs(): Promise<string[]> {
  const output = outputFromStoreResponse(await MatonOS.storeListInstalled());
  return output.split(/\r?\n/).map((line) => {
    const ref = line.trim();
    if (/^app\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(ref)) return ref;
    if (/^[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(ref)) return `app/${ref}`;
    return "";
  }).filter(Boolean);
}

export async function getRemotes(): Promise<string[]> {
  const output = outputFromStoreResponse(await MatonOS.storeListRemotes());
  return output.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
}
