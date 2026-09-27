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
const SUPER_BYTES = 10.5 * GiB;
const SLOT_CAPACITY = 5 * GiB;
const ALIGNMENT = MiB;
const GPT_BACKUP_RESERVE = MiB;
const USER_CONTENT = ["Documents", "Download", "Pictures", "Movies", "Music"];
const USER_EXCLUDES = [
  "Android/data/org.matonos.installer",
  "Android/obb/org.matonos.installer",
  "Android/media/org.matonos.installer",
  "data/misc",
  "data/system",
  "data/system_de",
  "data/misc/vold",
  "metadata",
  "misc",
  "adb_keys",
  "lost+found",
];

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
  return [
    { name: `system_${suffix}`, sizeBytes: 2048 * MiB },
    { name: `system_ext_${suffix}`, sizeBytes: 512 * MiB },
    { name: `product_${suffix}`, sizeBytes: 1536 * MiB },
    { name: `vendor_${suffix}`, sizeBytes: 960 * MiB },
    { name: `odm_${suffix}`, sizeBytes: 64 * MiB },
  ];
}

export type V1Plan = {
  operations: OperationRequestV1[];
  userdataBytes: number;
  partitions: GptPartition[];
};

export function createV1Plan(diskId: string, diskBytes: number): V1Plan {
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
  addPartition(
    "metadata",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    METADATA_BYTES,
  );
  const superPart = addPartition(
    "super",
    "0fc63daf-8483-4772-8e79-3d69d8477de4",
    SUPER_BYTES,
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
      target: { partGuid: userdata.partGuid },
      filesystem: "ext4" as const,
      label: "userdata",
    },
    {
      kind: "create_lp_metadata" as const,
      superPartGuid: superPart.partGuid,
      metadataSizeBytes: 64 * 1024,
      metadataSlots: 2,
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
          inlineContents: "default matonos-a.conf\ntimeout 5\neditor no\n",
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
      target: { partGuid: boot.partGuid },
      relativeDirectory: "matonos/a",
      files: [
        { relativePath: "bzImage", livePayloadPath: "live/boot/a/bzImage" },
        {
          relativePath: "vendor_ramdisk.img",
          livePayloadPath: "live/boot/a/vendor_ramdisk.img",
        },
        {
          relativePath: "ramdisk.img",
          livePayloadPath: "live/boot/a/ramdisk.img",
        },
      ],
    },
    ...["system", "system_ext", "product", "vendor", "odm"].map((name) => ({
      kind: "clone_partition" as const,
      source: { logicalName: `${name}_a` },
      target: { logicalName: `${name}_b` },
    })),
    {
      kind: "write_files" as const,
      target: { partGuid: boot.partGuid },
      relativeDirectory: "matonos/b",
      files: [
        { relativePath: "bzImage", livePayloadPath: "live/boot/a/bzImage" },
        {
          relativePath: "vendor_ramdisk.img",
          livePayloadPath: "live/boot/a/vendor_ramdisk.img",
        },
        {
          relativePath: "ramdisk.img",
          livePayloadPath: "live/boot/a/ramdisk.img",
        },
      ],
    },
    {
      kind: "write_files" as const,
      target: { partGuid: boot.partGuid },
      relativeDirectory: "loader/entries",
      files: [
        {
          relativePath: "matonos-a.conf",
          inlineContents: `title MatonOS (slot A)\nlinux /matonos/a/bzImage\ninitrd /matonos/a/vendor_ramdisk.img\ninitrd /matonos/a/ramdisk.img\noptions console=tty0 quiet androidboot.hardware=pc_x86_64 androidboot.boot_part_uuid=${esp.partGuid} androidboot.slot_suffix=_a androidboot.matonos.live=0\n`,
        },
        {
          relativePath: "matonos-b.conf",
          inlineContents: `title MatonOS (slot B)\nlinux /matonos/b/bzImage\ninitrd /matonos/b/vendor_ramdisk.img\ninitrd /matonos/b/ramdisk.img\noptions console=tty0 quiet androidboot.hardware=pc_x86_64 androidboot.boot_part_uuid=${esp.partGuid} androidboot.slot_suffix=_b androidboot.matonos.live=0\n`,
        },
      ],
    },
    {
      kind: "copy_user_files" as const,
      targetUserdata: { partGuid: userdata.partGuid },
      includePaths: USER_CONTENT,
      excludePaths: USER_EXCLUDES,
    },
    {
      kind: "write_files" as const,
      target: { partGuid: userdata.partGuid },
      relativeDirectory: "matonos/installer-state",
      files: [
        {
          relativePath: "dormant",
          inlineContents:
            "installer package disabled for installed image; installer service disabled\n",
        },
      ],
    },
  ] satisfies InstallerOperation[];
  return {
    operations: ops.map((operation) => ({
      apiVersion: 1,
      targetDiskId: diskId,
      operation,
    })),
    userdataBytes: userdata.sizeBytes,
    partitions,
  };
}
