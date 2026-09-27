export type DrivePartition = {
  name: string;
  partGuid: string;
  typeGuid: string;
  startBytes: number;
  sizeBytes: number;
  mounted: boolean;
  logical: boolean;
};

export type InstallDrive = {
  id: string;
  path: string;
  model: string;
  sizeBytes: number;
  transport: string;
  removable: boolean;
  safe: boolean;
  reason?: string;
  partitions: DrivePartition[];
};

export type PartitionRef =
  | { partGuid: string; logicalName?: never }
  | { logicalName: string; partGuid?: never };
export type GptPartition = {
  name: string;
  typeGuid: string;
  partGuid: string;
  startBytes: number;
  sizeBytes: number;
};
export type LogicalPartition = { name: string; sizeBytes: number };
export type LogicalGroup = {
  name: string;
  maximumSizeBytes: number;
  partitions: LogicalPartition[];
};
export type InstallerOperation =
  | {
      kind: "write_gpt";
      declaredDiskBytes: number;
      diskGuid: string;
      partitions: GptPartition[];
    }
  | {
      kind: "create_lp_metadata";
      superPartGuid: string;
      metadataSizeBytes: number;
      metadataSlots: number;
      groups: LogicalGroup[];
    }
  | {
      kind: "format";
      target: PartitionRef;
      filesystem: "vfat" | "ext4" | "f2fs";
      label: string;
    }
  | {
      kind: "copy_partition";
      source:
        | { kind: "live_image"; partitionName: string }
        | { kind: "target"; partition: PartitionRef };
      target: PartitionRef;
    }
  | { kind: "clone_partition"; source: PartitionRef; target: PartitionRef }
  | {
      kind: "write_files";
      target: PartitionRef;
      relativeDirectory: string;
      files: {
        relativePath: string;
        livePayloadPath?: string;
        inlineContents?: string;
        sha256?: string;
      }[];
    }
  | {
      kind: "copy_user_files";
      targetUserdata: PartitionRef;
      includePaths: string[];
      excludePaths: string[];
    };

export type OperationRequestV1 = {
  apiVersion: 1;
  targetDiskId: string;
  operation: InstallerOperation;
};
export type OperationProgress = {
  bytesDone: number;
  bytesTotal: number;
  message: string;
};
export type OperationResult = { ok: true } | { ok: false; message: string };

export type InstallerApi = {
  listDrives(): Promise<InstallDrive[]>;
  executeOperation(
    request: OperationRequestV1,
    onProgress: (progress: OperationProgress) => void,
  ): Promise<OperationResult>;
  cancelCurrentOperation(): Promise<boolean>;
};
