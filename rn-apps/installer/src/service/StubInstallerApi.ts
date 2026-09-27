import type {
  InstallDrive,
  InstallerApi,
  OperationRequestV1,
} from "./InstallerApi";

const GIB = 1024 ** 3;
let cancelled = false;

const demoDrives: InstallDrive[] = [
  {
    id: "8:16",
    path: "/dev/sdb",
    model: "Demo NVMe target",
    sizeBytes: 32 * GIB,
    transport: "NVMe",
    removable: false,
    safe: true,
    partitions: [],
  },
];

async function delay(ms: number): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, ms));
}

export const stubInstallerApi: InstallerApi = {
  async listDrives() {
    await delay(300);
    return demoDrives;
  },
  async executeOperation(request: OperationRequestV1, onProgress) {
    cancelled = false;
    const label = request.operation.kind.replaceAll("_", " ");
    for (let tick = 0; tick <= 4; tick += 1) {
      await delay(100);
      if (cancelled)
        return { ok: false, message: "Operation cancelled by user." };
      onProgress({
        bytesDone: tick,
        bytesTotal: 4,
        message: `Preview: ${label} (${tick * 25}%)`,
      });
    }
    return { ok: true };
  },
  async cancelCurrentOperation() {
    cancelled = true;
    return true;
  },
};
