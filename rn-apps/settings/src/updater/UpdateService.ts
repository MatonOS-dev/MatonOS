import { File } from "expo-file-system";
import {
  cacheDirectory,
  createDownloadResumable,
  deleteAsync,
  getInfoAsync,
  makeDirectoryAsync,
} from "expo-file-system/legacy";
import { MatonOS, type CallResult } from "../bridge/MatonOS";

const API = "https://maton.hanro50.net.za";
const CHANNEL = "main";
const STAGE_NAMES = [
  "manifest.json",
  "manifest.sig",
  "system.erofs",
  "system_ext.erofs",
  "product.erofs",
  "vendor.erofs",
  "odm.erofs",
  "vbmeta.img",
  "matonos-a.efi",
  "matonos-b.efi",
  "matonos-a-hdd.efi",
  "matonos-b-hdd.efi",
] as const;
const PAYLOAD_NAMES = STAGE_NAMES.slice(2);

export type UpdateProgress = {
  step: string;
  file: string;
  bytesDone: number;
  bytesTotal: number;
};
export type UpdateCheck =
  | { kind: "up-to-date" }
  | { kind: "not-offered"; reason: string }
  | {
      kind: "available";
      id: number;
      version: string;
      files: UpdateFile[];
      manifestUrl: string;
      signatureUrl: string;
    };
export type UpdateFile = {
  name: string;
  url: string;
  size: number;
  sha256: string;
};
export type UpdateStatus = { slot: string; available: boolean };

function fail(step: string, message: string): Error {
  return new Error(`${step}: ${message}`);
}

function parseJson<T>(step: string, value: string): T {
  try {
    return JSON.parse(value) as T;
  } catch (error) {
    throw fail(step, error instanceof Error ? error.message : String(error));
  }
}

function requireCall(step: string, result: CallResult): string {
  if (!result.available)
    throw fail(step, result.reason || "The system bridge call failed.");
  return result.value;
}

function installedBase(version: string): number {
  const match = /^(\d+)/.exec(version);
  if (!match)
    throw fail(
      "Read installed version",
      `Cannot parse Android release ${version}.`,
    );
  return Number(match[1]);
}

async function installedAddons(): Promise<string[]> {
  let response: CallResult;
  try {
    response = await MatonOS.call("addons", "status");
  } catch (error) {
    throw fail(
      "Read installed add-ons",
      error instanceof Error ? error.message : String(error),
    );
  }
  const status = parseJson<{ addons?: Array<{ id?: unknown }> }>(
    "Read installed add-ons",
    requireCall("Read installed add-ons", response),
  );
  if (!Array.isArray(status.addons))
    throw fail(
      "Read installed add-ons",
      "The add-ons service returned no inventory.",
    );
  return status.addons.map((addon) => {
    if (typeof addon.id !== "string" || addon.id.length === 0) {
      throw fail(
        "Read installed add-ons",
        "The add-ons service returned an invalid add-on ID.",
      );
    }
    return addon.id;
  });
}

function checkedUrl(path: string): string {
  const url = new URL(path, API);
  if (url.origin !== API || url.protocol !== "https:") {
    throw fail(
      "Check for updates",
      `The server returned an unsafe URL: ${path}`,
    );
  }
  return url.toString();
}

async function postJson<T>(path: string, body: unknown): Promise<T> {
  let response: Response;
  try {
    response = await fetch(checkedUrl(path), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  } catch (error) {
    throw fail(
      `Request ${path}`,
      error instanceof Error ? error.message : String(error),
    );
  }
  const text = await response.text();
  const value = parseJson<Record<string, unknown>>(`Request ${path}`, text);
  if (!response.ok) {
    const message =
      typeof value.message === "string" ? value.message : response.statusText;
    throw fail(`Request ${path}`, message || `HTTP ${response.status}`);
  }
  return value as T;
}

export async function checkForUpdate(
  androidVersion: string,
): Promise<UpdateCheck> {
  const addons = await installedAddons();
  const response = await postJson<{
    available: boolean;
    reason?: string;
    update_id?: number;
    version?: string;
    manifest_url?: string;
    signature_url?: string;
    files?: UpdateFile[];
  }>("/api/update/check", {
    channel: CHANNEL,
    base_android: installedBase(androidVersion),
    addon_list: addons,
  });
  if (!response.available) {
    if (response.reason)
      return { kind: "not-offered", reason: response.reason };
    return { kind: "up-to-date" };
  }
  if (
    !Number.isSafeInteger(response.update_id) ||
    typeof response.version !== "string"
  ) {
    throw fail(
      "Check for updates",
      "The server returned an invalid update identity.",
    );
  }
  if (
    !response.manifest_url ||
    !response.signature_url ||
    !Array.isArray(response.files)
  ) {
    throw fail(
      "Check for updates",
      "The server returned an incomplete update inventory.",
    );
  }
  const files = response.files;
  const names = new Set<string>();
  for (const file of files) {
    if (
      typeof file.name !== "string" ||
      !PAYLOAD_NAMES.includes(file.name as (typeof PAYLOAD_NAMES)[number]) ||
      names.has(file.name) ||
      typeof file.url !== "string" ||
      !Number.isSafeInteger(file.size) ||
      file.size < 0 ||
      !/^[a-f0-9]{64}$/i.test(file.sha256)
    ) {
      throw fail(
        "Check for updates",
        "The server returned an invalid file record.",
      );
    }
    names.add(file.name);
    file.url = checkedUrl(file.url);
  }
  const missing = PAYLOAD_NAMES.filter((name) => !names.has(name));
  if (missing.length > 0)
    throw fail(
      "Check for updates",
      `Missing required files: ${missing.join(", ")}`,
    );
  return {
    kind: "available",
    id: response.update_id as number,
    version: response.version,
    files,
    manifestUrl: checkedUrl(response.manifest_url),
    signatureUrl: checkedUrl(response.signature_url),
  };
}

export async function readUpdateStatus(): Promise<UpdateStatus> {
  let result: CallResult;
  try {
    result = await MatonOS.getUpdateStatus();
  } catch (error) {
    throw fail(
      "Read update status",
      error instanceof Error ? error.message : String(error),
    );
  }
  const parsed = parseJson<Record<string, unknown>>(
    "Read update status",
    requireCall("Read update status", result),
  );
  const slot = parsed.runningSlot ?? parsed.slot;
  if (typeof slot !== "string")
    throw fail("Read update status", "The installer returned no running slot.");
  if (typeof parsed.available !== "boolean")
    throw fail(
      "Read update status",
      "The installer returned no staged-update status.",
    );
  return { slot, available: parsed.available };
}

export async function installUpdate(
  update: Extract<UpdateCheck, { kind: "available" }>,
  onProgress: (progress: UpdateProgress) => void,
): Promise<void> {
  if (!cacheDirectory)
    throw fail("Prepare download", "The app cache directory is unavailable.");
  const directory = `${cacheDirectory}matonos-ota-${update.id}/`;
  try {
    const directoryInfo = await getInfoAsync(directory);
    if (directoryInfo.exists)
      await deleteAsync(directory, { idempotent: true });
    await makeDirectoryAsync(directory, { intermediates: true });
  } catch (error) {
    throw fail(
      "Prepare download",
      error instanceof Error ? error.message : String(error),
    );
  }

  const totalBytes = update.files.reduce((total, file) => total + file.size, 0);
  let completedBytes = 0;
  const filesByName = new Map<string, string>();
  const downloadItems = [
    {
      name: "manifest.json",
      url: update.manifestUrl,
      size: null,
      sha256: null,
    },
    {
      name: "manifest.sig",
      url: update.signatureUrl,
      size: null,
      sha256: null,
    },
    ...update.files,
  ];
  for (const item of downloadItems) {
    onProgress({
      step: "Download",
      file: item.name,
      bytesDone: completedBytes,
      bytesTotal: totalBytes,
    });
    const destination = `${directory}${item.name}`;
    const task = createDownloadResumable(
      item.url,
      destination,
      {},
      (progress) => {
        const current =
          item.size === null
            ? 0
            : Math.min(progress.totalBytesWritten, item.size);
        onProgress({
          step: "Download",
          file: item.name,
          bytesDone: completedBytes + current,
          bytesTotal: totalBytes,
        });
      },
    );
    let downloaded: Awaited<ReturnType<typeof task.downloadAsync>>;
    try {
      downloaded = await task.downloadAsync();
    } catch (error) {
      throw fail(
        `Download ${item.name}`,
        error instanceof Error ? error.message : String(error),
      );
    }
    if (!downloaded?.uri)
      throw fail(
        `Download ${item.name}`,
        "The download did not produce a local file.",
      );
    filesByName.set(item.name, downloaded.uri);
    if (item.size !== null) {
      let info: Awaited<ReturnType<typeof getInfoAsync>>;
      try {
        info = await getInfoAsync(downloaded.uri);
      } catch (error) {
        throw fail(
          `Verify ${item.name}`,
          error instanceof Error ? error.message : String(error),
        );
      }
      if (!info.exists || info.size !== item.size) {
        throw fail(
          `Verify ${item.name}`,
          `Expected ${item.size} bytes; received ${info.exists ? info.size : "no file"}.`,
        );
      }
      onProgress({
        step: "Verify",
        file: item.name,
        bytesDone: completedBytes,
        bytesTotal: totalBytes,
      });
      let hash: string;
      try {
        hash = await sha256File(downloaded.uri);
      } catch (error) {
        throw fail(
          `Verify ${item.name}`,
          error instanceof Error ? error.message : String(error),
        );
      }
      if (hash.toLowerCase() !== item.sha256.toLowerCase()) {
        throw fail(
          `Verify ${item.name}`,
          `SHA-256 mismatch: expected ${item.sha256}, got ${hash}.`,
        );
      }
      completedBytes += item.size;
    }
  }

  const uris = STAGE_NAMES.map((name) => {
    const uri = filesByName.get(name);
    if (!uri) throw fail("Stage update", `No downloaded file for ${name}.`);
    return uri;
  });
  onProgress({
    step: "Stage update",
    file: "",
    bytesDone: totalBytes,
    bytesTotal: totalBytes,
  });
  let staged: CallResult;
  try {
    staged = await MatonOS.stageUpdate([...STAGE_NAMES], uris);
  } catch (error) {
    throw fail(
      "Stage update",
      error instanceof Error ? error.message : String(error),
    );
  }
  requireCall("Stage update", staged);
  onProgress({
    step: "Finalize update",
    file: "",
    bytesDone: totalBytes,
    bytesTotal: totalBytes,
  });
  let finalized: CallResult;
  try {
    finalized = await MatonOS.finalizeUpdate();
  } catch (error) {
    throw fail(
      "Finalize update",
      error instanceof Error ? error.message : String(error),
    );
  }
  requireCall("Finalize update", finalized);
}

// Streaming SHA-256 keeps multi-gigabyte system images out of the JS heap.
async function sha256File(uri: string): Promise<string> {
  const file = new File(uri);
  const handle = file.open();
  const hash = new Sha256();
  try {
    while (
      handle.offset !== null &&
      handle.size !== null &&
      handle.offset < handle.size
    ) {
      hash.update(
        handle.readBytes(Math.min(1024 * 1024, handle.size - handle.offset)),
      );
    }
  } finally {
    handle.close();
  }
  return hash.digest();
}

class Sha256 {
  private state = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
    0x1f83d9ab, 0x5be0cd19,
  ]);
  private buffer = new Uint8Array(64);
  private buffered = 0;
  private bytes = 0;
  private readonly constants = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);

  update(input: Uint8Array) {
    this.bytes += input.length;
    let offset = 0;
    while (offset < input.length) {
      const count = Math.min(64 - this.buffered, input.length - offset);
      this.buffer.set(input.subarray(offset, offset + count), this.buffered);
      this.buffered += count;
      offset += count;
      if (this.buffered === 64) {
        this.compress(this.buffer);
        this.buffered = 0;
      }
    }
  }

  digest(): string {
    const bitLength = this.bytes * 8;
    this.buffer[this.buffered++] = 0x80;
    if (this.buffered > 56) {
      this.buffer.fill(0, this.buffered);
      this.compress(this.buffer);
      this.buffered = 0;
    }
    this.buffer.fill(0, this.buffered, 56);
    const view = new DataView(this.buffer.buffer);
    view.setUint32(56, Math.floor(bitLength / 0x100000000), false);
    view.setUint32(60, bitLength >>> 0, false);
    this.compress(this.buffer);
    return Array.from(this.state, (value) =>
      value.toString(16).padStart(8, "0"),
    ).join("");
  }

  private compress(block: Uint8Array) {
    const words = new Uint32Array(64);
    const view = new DataView(block.buffer, block.byteOffset, block.byteLength);
    for (let index = 0; index < 16; index += 1)
      words[index] = view.getUint32(index * 4, false);
    for (let index = 16; index < 64; index += 1) {
      const x = words[index - 15];
      const y = words[index - 2];
      const s0 = rotate(x, 7) ^ rotate(x, 18) ^ (x >>> 3);
      const s1 = rotate(y, 17) ^ rotate(y, 19) ^ (y >>> 10);
      words[index] = (words[index - 16] + s0 + words[index - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, h] = this.state;
    for (let index = 0; index < 64; index += 1) {
      const sum1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
      const choice = (e & f) ^ (~e & g);
      const temp1 =
        (h + sum1 + choice + this.constants[index] + words[index]) >>> 0;
      const sum0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const temp2 = (sum0 + majority) >>> 0;
      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }
    this.state[0] = (this.state[0] + a) >>> 0;
    this.state[1] = (this.state[1] + b) >>> 0;
    this.state[2] = (this.state[2] + c) >>> 0;
    this.state[3] = (this.state[3] + d) >>> 0;
    this.state[4] = (this.state[4] + e) >>> 0;
    this.state[5] = (this.state[5] + f) >>> 0;
    this.state[6] = (this.state[6] + g) >>> 0;
    this.state[7] = (this.state[7] + h) >>> 0;
  }
}

function rotate(value: number, bits: number): number {
  return (value >>> bits) | (value << (32 - bits));
}
