import type {
  GptPartition,
  InstallerOperation,
  OperationRequestV1,
} from "./InstallerApi";

const MiB = 1024 ** 2;
const GiB = 1024 ** 3;
export const V1_MINIMUM_DISK_BYTES = 32 * GiB;

// These are installer-app policy. The primitive service deliberately knows none of them.
const ESP_BYTES = 2 * GiB;
const MATON_SYSTEM_BYTES = 16 * GiB;
const METADATA_BYTES = 64 * MiB;
const ALIGNMENT = MiB;
const GPT_BACKUP_RESERVE = MiB;

function align(value: number): number {
  return Math.ceil(value / ALIGNMENT) * ALIGNMENT;
}

function guid(): string {
  // UUIDs are identifiers, not secrets; native executor still validates uniqueness and bounds.
  const bytes = Array.from({ length: 16 }, () =>
    Math.floor(Math.random() * 256),
  );
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = bytes
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function bootDeviceFromIdentity(identity: string): string {
  const sys = identity.match(/^sys=(.*?);seq=/)?.[1];
  if (!sys?.startsWith("/sys/devices/"))
    throw new Error("Refresh the drive list; the target boot path is unavailable.");
  const relative = sys.slice("/sys".length);
  const pci = relative.match(/^\/devices\/(pci[^/]+\/[^/]+)/);
  if (pci) return pci[1];
  const platform = relative.match(/^\/devices\/platform\/(.+)\/(?:ata|host|nvme|mmc_host)[^/]*\//);
  if (platform) return platform[1];
  throw new Error("This target's firmware device path is not supported yet.");
}

export type V1Plan = {
  operations: OperationRequestV1[];
  userdataBytes: number;
  partitions: GptPartition[];
};

export function createV1Plan(
  diskId: string,
  diskIdentity: string,
  diskBytes: number,
  rotational = false,
): V1Plan {
  if (!diskIdentity)
    throw new Error("Refresh the drive list before confirming this target.");
  const bootDevice = bootDeviceFromIdentity(diskIdentity);
  if (diskBytes < V1_MINIMUM_DISK_BYTES)
    throw new Error("This installer profile requires a 32 GiB drive.");
  let cursor = ALIGNMENT;
  const partitions: GptPartition[] = [];
  const addPartition = (name: string, typeGuid: string, sizeBytes: number) => {
    cursor = align(cursor);
    const part = {
      name,
      typeGuid,
      partGuid: guid(),
      startBytes: cursor,
      sizeBytes,
    };
    partitions.push(part);
    cursor += sizeBytes;
    return part;
  };
  const esp = addPartition(
    "esp",
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",
    ESP_BYTES,
  );
  const matonSystem = addPartition(
    "maton_system",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    MATON_SYSTEM_BYTES,
  );
  const metadata = addPartition(
    "metadata",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    METADATA_BYTES,
  );
  const userdataStart = align(cursor);
  const userdataEnd =
    Math.floor((diskBytes - GPT_BACKUP_RESERVE) / ALIGNMENT) * ALIGNMENT;
  const userdata = addPartition(
    "userdata",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    userdataEnd - userdataStart,
  );
  const diskGuid = guid();
  const ops = [
    {
      kind: "write_gpt" as const,
      declaredDiskBytes: diskBytes,
      diskGuid,
      partitions,
    },
    {
      kind: "format" as const,
      target: { partGuid: esp.partGuid },
      filesystem: "vfat" as const,
      label: "MATON_ESP",
    },
    {
      kind: "format" as const,
      target: { partGuid: metadata.partGuid },
      filesystem: "ext4" as const,
      label: "metadata",
    },
    {
      kind: "format" as const,
      target: { partGuid: userdata.partGuid },
      filesystem: rotational ? "ext4" as const : "f2fs" as const,
      label: "userdata",
    },
    {
      kind: "populate_images" as const,
      matonSystemPartGuid: matonSystem.partGuid,
      metadataPartGuid: metadata.partGuid,
      images: ["system", "system_ext", "product", "vendor", "odm"].map((name) => ({
        name: `${name}_a`,
        livePartitionName: `${name}_a`,
      })),
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "EFI",
      files: [
        { relativePath: "BOOT/BOOTX64.EFI", livePayloadPath: "live/esp/EFI/BOOT/BOOTX64.EFI" },
        { relativePath: "BOOT/grubx64.efi", livePayloadPath: "live/esp/EFI/BOOT/grubx64.efi" },
        { relativePath: "systemd/systemd-bootx64.efi", livePayloadPath: "live/esp/EFI/systemd/systemd-bootx64.efi" },
      ],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "EFI/Linux",
      files: [{ relativePath: "matonos-installed.efi", livePayloadPath: "live/esp/EFI/Linux/matonos-installed-a.efi" }],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "loader",
      files: [{ relativePath: "loader.conf", inlineContents: "default matonos.conf\ntimeout 5\neditor no\n" }],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "loader/entries",
      files: [{
        relativePath: "matonos.conf",
        inlineContents: `title MatonOS\nefi /EFI/Linux/matonos-installed.efi\noptions androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_a androidboot.boot_part_uuid=${esp.partGuid} androidboot.boot_devices=${bootDevice} androidboot.matonos.live=0 androidboot.selinux=enforcing androidboot.verifiedbootstate=orange firmware_class.path=/vendor/firmware console=ttyS0,115200 console=tty0 audit_backlog_limit=8192 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6\n`,
      }],
    },
  ] satisfies InstallerOperation[];
  return {
    operations: ops.map((operation) => ({
      apiVersion: 1,
      targetDiskId: diskId,
      targetDiskIdentity: diskIdentity,
      operation,
    })),
    userdataBytes: userdata.sizeBytes,
    partitions,
  };
}
