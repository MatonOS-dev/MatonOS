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

export async function installApp(ref: string, operationId: string) {
  await callFlatpak("add_flathub");
  return callFlatpak("install", { ref, operationId });
}

export async function uninstallApp(ref: string, operationId: string, deleteData = false) {
  return callFlatpak("uninstall", { ref, operationId, deleteData });
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
