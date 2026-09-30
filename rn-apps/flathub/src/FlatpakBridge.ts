import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";

export type ProgressEvent = { line?: string; percent?: number; phase?: string; result?: { ok?: boolean; error?: string; output?: string } };

function decode(value: string): { ok?: boolean; accepted?: boolean; output?: string; error?: string } {
  try { return JSON.parse(value) as { ok?: boolean; accepted?: boolean; output?: string; error?: string }; }
  catch { throw new Error("The Flatpak service returned an invalid response."); }
}

export async function callFlatpak(command: string, args: unknown = {}) {
  const result = await MatonOS.call("flatpak", command, args);
  if (!result.available) throw new Error(result.reason || "The Flatpak service is unavailable.");
  const response = decode(result.value);
  if (!response.ok) {
    const output = response.output?.trim();
    throw new Error(response.error || output || "The Flatpak operation failed.");
  }
  return response;
}

export async function installApp(ref: string) {
  await callFlatpak("add_flathub");
  return callFlatpak("install", { ref });
}

export async function uninstallApp(ref: string) {
  return callFlatpak("uninstall", { ref });
}

export async function runApp(appId: string) {
  return callFlatpak("run", { appId });
}

export async function getInstalledRefs(): Promise<string[]> {
  const response = await callFlatpak("list_installed");
  const output = response.output || "";
  return output.split(/\r?\n/).map((line) => line.trim().split(/[\t ]+/).find((part) => /^app\/[A-Za-z0-9._-]+\//.test(part)))
    .filter((ref): ref is string => Boolean(ref));
}
