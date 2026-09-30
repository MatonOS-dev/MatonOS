import type {
  GptPartition,
  InstallerOperation,
  OperationRequestV1,
} from "../service/InstallerApi";

const MiB = 1024 ** 2;
const GiB = 1024 ** 3;
export const V1_MINIMUM_DISK_BYTES = 16 * GiB;

// These are installer-app policy. The primitive service deliberately knows none of them.
const ESP_BYTES = 512 * MiB;
const XBOOTLDR_BYTES = 1024 * MiB;
const MISC_BYTES = 4 * MiB;
const METADATA_BYTES = 64 * MiB;
const SUPER_BYTES = 6 * GiB;
const SLOT_CAPACITY = 5 * GiB;
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

function slotMembers(suffix: "a" | "b") {
  const size = (value: number) => (suffix === "a" ? value * MiB : 0);
  return [
    { name: `system_${suffix}`, sizeBytes: size(2048) },
    { name: `system_ext_${suffix}`, sizeBytes: size(512) },
    { name: `product_${suffix}`, sizeBytes: size(1536) },
    { name: `vendor_${suffix}`, sizeBytes: size(960) },
    { name: `odm_${suffix}`, sizeBytes: size(64) },
  ];
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
): V1Plan {
  if (!diskIdentity)
    throw new Error("Refresh the drive list before confirming this target.");
  const bootDevice = bootDeviceFromIdentity(diskIdentity);
  if (diskBytes < V1_MINIMUM_DISK_BYTES)
    throw new Error("This installer profile requires a 16 GiB drive.");
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
  const boot = addPartition(
    "boot",
    "bc13c2ff-59e6-4262-a352-b275fd6f7172",
    XBOOTLDR_BYTES,
  );
  addPartition("misc", "0fc63daf-8483-4772-8e79-3d69d8477de4", MISC_BYTES);
  const metadata = addPartition(
    "metadata",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    METADATA_BYTES,
  );
  const superPart = addPartition(
    "super",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    SUPER_BYTES,
  );
  const addonsA = addPartition(
    "addons_a",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    512 * MiB,
  );
  const addonsB = addPartition(
    "addons_b",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    512 * MiB,
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
      target: { partGuid: boot.partGuid },
      filesystem: "vfat" as const,
      label: "MATON_BOOT",
    },
    {
      kind: "format" as const,
      target: { partGuid: metadata.partGuid },
      filesystem: "ext4" as const,
      label: "metadata",
    },
    {
      kind: "format" as const,
      target: { partGuid: addonsA.partGuid },
      filesystem: "ext4" as const,
      label: "addons_a",
    },
    {
      kind: "format" as const,
      target: { partGuid: addonsB.partGuid },
      filesystem: "ext4" as const,
      label: "addons_b",
    },
    {
      kind: "format" as const,
      target: { partGuid: userdata.partGuid },
      filesystem: "ext4" as const,
      label: "userdata",
    },
    {
      kind: "create_lp_metadata" as const,
      superPartGuid: superPart.partGuid,
      metadataSizeBytes: 64 * 1024,
      metadataSlots: 3,
      groups: [
        {
          name: "pc_dynamic_partitions_a",
          maximumSizeBytes: SLOT_CAPACITY,
          partitions: slotMembers("a"),
        },
        {
          name: "pc_dynamic_partitions_b",
          maximumSizeBytes: SLOT_CAPACITY,
          partitions: slotMembers("b"),
        },
      ],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "EFI",
      files: [
        {
          relativePath: "BOOT/BOOTX64.EFI",
          livePayloadPath: "live/esp/EFI/BOOT/BOOTX64.EFI",
        },
        {
          relativePath: "BOOT/grubx64.efi",
          livePayloadPath: "live/esp/EFI/BOOT/grubx64.efi",
        },
        {
          relativePath: "systemd/systemd-bootx64.efi",
          livePayloadPath: "live/esp/EFI/systemd/systemd-bootx64.efi",
        },
      ],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "loader",
      files: [
        {
          relativePath: "loader.conf",
          inlineContents: "default A+3-0.conf\ntimeout 5\neditor no\n",
        },
      ],
    },
    ...["system", "system_ext", "product", "vendor", "odm"].map((name) => ({
      kind: "copy_partition" as const,
      source: { kind: "live_image" as const, partitionName: `${name}_a` },
      target: { logicalName: `${name}_a` },
    })),
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "EFI/Linux",
      files: [
        { relativePath: "matonos-a.efi", livePayloadPath: "live/esp/EFI/Linux/matonos-installed-a.efi" },
        { relativePath: "matonos-b.efi", livePayloadPath: "live/esp/EFI/Linux/matonos-installed-b.efi" },
      ],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: esp.partGuid },
      relativeDirectory: "loader/entries",
      files: [
        {
          relativePath: "A+3-0.conf",
          inlineContents: `title MatonOS (slot A)\nefi /EFI/Linux/matonos-a.efi\noptions androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_a androidboot.boot_part_uuid=${esp.partGuid} androidboot.boot_devices=${bootDevice} androidboot.matonos.live=0 androidboot.selinux=permissive androidboot.verifiedbootstate=orange firmware_class.path=/vendor/firmware console=ttyS0,115200 console=tty0 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6\n`,
        },
        {
          // systemd-boot ignores this extension until bootctrl atomically
          // restores it as B+3-0.conf after an OTA has populated slot B.
          relativePath: "B.DIS",
          inlineContents: `title MatonOS (slot B)\nefi /EFI/Linux/matonos-b.efi\noptions androidboot.hardware=pc_x86_64 androidboot.fstab_suffix=pc_x86_64 androidboot.slot_suffix=_b androidboot.boot_part_uuid=${esp.partGuid} androidboot.boot_devices=${bootDevice} androidboot.matonos.live=0 androidboot.selinux=permissive androidboot.verifiedbootstate=orange firmware_class.path=/vendor/firmware console=ttyS0,115200 console=tty0 quiet loglevel=3 vt.global_cursor_default=0 fbcon=vc:2-6\n`,
        },
      ],
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
