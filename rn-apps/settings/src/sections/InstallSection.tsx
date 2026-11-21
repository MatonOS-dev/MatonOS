import { useCallback, useRef, useState } from "react";
import type { ReactNode } from "react";
import { useFocusEffect } from "expo-router";
import {
  Column,
  LinearProgressIndicator,
  Text,
  useMaterialColors,
} from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import { bridgeInstallerApi } from "../installer/BridgeInstallerApi";
import {
  type InstallDrive,
  type InstallerOperation,
  type OperationProgress,
} from "../installer/InstallerApi";
import { createV1Plan, V1_MINIMUM_DISK_BYTES } from "../installer/createV1Plan";
import { useMatonSettings } from "../context/MatonSettingsContext";
import { SectionHeader } from "../components/SettingsUI";
import { copy, numberFormat } from "../strings";
import {
  InstallConfirmStep,
  InstallDoneStep,
  InstallDrivesStep,
  InstallErrorStep,
  InstallProgressStep,
  InstallWelcomeStep,
  type InstallStep,
  type OperationState,
} from "./install/InstallSteps";

type ReadyState = "loading" | "ready" | "unavailable";

function sizeLabel(bytes: number) {
  const gib = bytes / 1024 ** 3;
  return gib >= 0.1 ? `${numberFormat.format(gib)} GiB` : `${numberFormat.format(bytes / 1024 ** 2)} MiB`;
}

function parse<T>(value: string): T | null {
  try {
    return JSON.parse(value) as T;
  } catch {
    return null;
  }
}

const formatTitles: Record<string, string> = {
  MATON_ESP: "Format the boot partition",
  metadata: "Format the encryption metadata partition",
};

const writeFilesTitles: Record<string, string> = {
  EFI: "Write the boot loaders",
  "EFI/Linux": "Write the MatonOS boot image",
  loader: "Write the boot menu settings",
  "loader/entries": "Write the MatonOS boot entry",
};

function operationTitle(operation: InstallerOperation) {
  if (operation.kind === "format") {
    return formatTitles[operation.label] ?? `Format the ${operation.label} partition`;
  }
  if (operation.kind === "write_files") {
    return (
      writeFilesTitles[operation.relativeDirectory] ??
      `Write boot files to ${operation.relativeDirectory}`
    );
  }
  const labels: Record<InstallerOperation["kind"], string> = {
    write_gpt: "Trim the drive and write the partition table",
    create_lp_metadata: "Create dynamic partition metadata",
    populate_images: "Install system images",
    format: "Format a target partition",
    copy_partition: "Copy a system partition",
    clone_partition: "Clone a system partition",
    write_files: "Write boot and loader files",
    register_boot_entry: "Add MatonOS to the firmware boot menu",
  };
  return labels[operation.kind];
}

function ProgressSummary({
  step,
  progress,
}: {
  step: InstallStep;
  progress: OperationProgress | null;
}) {
  const colors = useMaterialColors();
  if (step !== "progress" || !progress) {
    return null;
  }
  return (
    <>
      <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
        {progress.message}
      </Text>
      <LinearProgressIndicator
        progress={
          progress.bytesTotal > 0
            ? progress.bytesDone / progress.bytesTotal
            : null
        }
      />
    </>
  );
}

export function InstallSection() {
  const { live } = useMatonSettings();
  const [step, setStep] = useState<InstallStep>("welcome");
  const [drives, setDrives] = useState<InstallDrive[]>([]);
  const [selected, setSelected] = useState<InstallDrive | null>(null);
  const [eraseConfirmed, setEraseConfirmed] = useState(false);
  const [healthAccepted, setHealthAccepted] = useState(false);
  const [readiness, setReadiness] = useState<ReadyState>("loading");
  const [loadError, setLoadError] = useState("");
  const [operationStates, setOperationStates] = useState<OperationState[]>([]);
  const [progress, setProgress] = useState<OperationProgress | null>(null);
  const [failedOperation, setFailedOperation] = useState("");
  const [failureMessage, setFailureMessage] = useState("");
  const [planCount, setPlanCount] = useState(0);
  const [previewMode, setPreviewMode] = useState(false);
  const previewModeRef = useRef(false);

  const refreshDrives = useCallback(async () => {
    setReadiness("loading");
    setLoadError("");
    try {
      const state = await MatonOS.call("install", "get_status");
      if (previewModeRef.current) return;
      if (!state.available)
        throw new Error(state.reason || copy.installServiceUnavailable);
      const status = parse<{ executorReady: boolean }>(state.value);
      if (!status?.executorReady) throw new Error(copy.installUnavailable);
      const availableDrives = await bridgeInstallerApi.listDrives();
      if (previewModeRef.current) return;
      setDrives(availableDrives);
      setReadiness("ready");
    } catch (problem) {
      if (previewModeRef.current) return;
      setReadiness("unavailable");
      setLoadError(
        problem instanceof Error
          ? problem.message
          : copy.installServiceUnavailable,
      );
      setDrives([]);
    }
  }, []);

  useFocusEffect(
    useCallback(() => {
      if (live) void refreshDrives();
    }, [live, refreshDrives]),
  );

  const startInstall = async () => {
    if (!selected) return;
    if (previewMode) {
      const demoOperations: OperationState[] = [
        { title: "Write the target partition table", status: "complete", progress: null },
        { title: "Format target filesystems", status: "complete", progress: null },
        { title: "Install system images", status: "running", progress: null },
        { title: "Write boot and loader files", status: "waiting", progress: null },
      ];
      setPlanCount(demoOperations.length);
      setOperationStates(demoOperations);
      setFailureMessage("");
      setFailedOperation("");
      setStep("progress");
      setProgress({
        bytesDone: 256 * 1024 ** 2,
        bytesTotal: 1024 * 1024 ** 2,
        message: "Preview: copying the live system",
      });
      return;
    }

    let operations: ReturnType<typeof createV1Plan>["operations"];
    try {
      operations = createV1Plan(
        selected.id,
        selected.identity,
        selected.sizeBytes,
        selected.rotational,
      ).operations;
    } catch (problem) {
      setFailedOperation("Prepare the install plan");
      setFailureMessage(
        problem instanceof Error ? problem.message : copy.invalidTarget,
      );
      setStep("error");
      return;
    }

    setPlanCount(operations.length);
    setOperationStates(
      operations.map((request) => ({
        title: operationTitle(request.operation),
        status: "waiting",
        progress: null,
      })),
    );
    setFailureMessage("");
    setFailedOperation("");
    setStep("progress");

    for (let index = 0; index < operations.length; index += 1) {
      const request = operations[index];
      const title = operationTitle(request.operation);
      setOperationStates((current) =>
        current.map((item, itemIndex) =>
          itemIndex === index ? { ...item, status: "running" } : item,
        ),
      );
      setProgress({
        bytesDone: 0,
        bytesTotal: 0,
        message: `${index + 1}/${operations.length}: ${title}`,
      });
      try {
        const result = await bridgeInstallerApi.executeOperation(
          request,
          (currentProgress) => {
            setProgress(currentProgress);
            setOperationStates((current) =>
              current.map((item, itemIndex) =>
                itemIndex === index
                  ? { ...item, progress: currentProgress }
                  : item,
              ),
            );
          },
        );
        if (!result.ok) throw new Error(result.message);
        setOperationStates((current) =>
          current.map((item, itemIndex) =>
            itemIndex === index
              ? { ...item, status: "complete", progress: null }
              : item,
          ),
        );
      } catch (problem) {
        setFailedOperation(title);
        setFailureMessage(
          problem instanceof Error ? problem.message : copy.installFailed,
        );
        setOperationStates((current) =>
          current.map((item, itemIndex) =>
            itemIndex === index ? { ...item, status: "failed" } : item,
          ),
        );
        setProgress(null);
        setStep("error");
        return;
      }
    }
    setProgress(null);
    setStep("done");
  };

  const rebootNow = useCallback(() => {
    MatonOS.reboot().catch((error: unknown) => {
      console.error("Reboot after install failed", error);
      setFailedOperation("Reboot");
      setFailureMessage(String(error));
      setStep("error");
    });
  }, []);
  const restartWizard = () => {
    previewModeRef.current = false;
    setSelected(null);
    setEraseConfirmed(false);
    setHealthAccepted(false);
    setOperationStates([]);
    setProgress(null);
    setFailedOperation("");
    setFailureMessage("");
    setPreviewMode(false);
    setStep("welcome");
    void refreshDrives();
  };

  const steps: Record<InstallStep, () => ReactNode> = {
    welcome: () => (
      <InstallWelcomeStep
        readiness={readiness}
        message={loadError}
        minimumSize={sizeLabel(V1_MINIMUM_DISK_BYTES)}
        onContinue={() => setStep("drives")}
        onRefresh={() => void refreshDrives()}
        onPreview={() => {
          previewModeRef.current = true;
          const previewDrive: InstallDrive = {
            id: "preview-drive",
            identity:
              "sys=/sys/devices/pci0000:00/0000:00:01.0;seq=1;size=21474836480",
            path: "/dev/block/sdb",
            model: "Preview USB drive",
            sizeBytes: 20 * 1024 ** 3,
            transport: "USB",
            removable: true,
            safe: true,
            rotational: false,
            health: {
              known: true,
              source: "ata",
              findings: [
                { level: "error", message: "This drive reports 8 unreadable sectors waiting to be remapped." },
                { level: "warning", message: "This SSD has used about 80% of its write cycles." },
              ],
            },
            partitions: [],
          };
          setPreviewMode(true);
          setDrives([previewDrive]);
          setSelected(null);
          setReadiness("ready");
          setLoadError("");
          setStep("drives");
        }}
      />
    ),
    drives: () => (
      <InstallDrivesStep
        drives={drives}
        preview={previewMode}
        error={loadError}
        ready={readiness === "ready"}
        selectedId={selected?.id ?? ""}
        onSelect={(drive) => {
          setSelected(drive);
          setHealthAccepted(false);
          setStep("confirm");
        }}
        onRefresh={() => void refreshDrives()}
        formatSize={sizeLabel}
      />
    ),
    confirm: () => (
      <InstallConfirmStep
        drive={selected}
        confirmed={eraseConfirmed}
        preview={previewMode}
        onConfirmChange={setEraseConfirmed}
        healthAccepted={healthAccepted}
        onHealthAcceptedChange={setHealthAccepted}
        onBack={() => setStep("drives")}
        onStart={() => void startInstall()}
        formatSize={sizeLabel}
      />
    ),
    progress: () => (
      <InstallProgressStep
        operations={operationStates}
        progress={progress}
        count={planCount}
        preview={previewMode}
        onPreviewComplete={() => {
          setOperationStates((current) =>
            current.map((item) => ({ ...item, status: "complete", progress: null })),
          );
          setProgress(null);
          setStep("done");
        }}
        onPreviewFailure={() => {
          const active = operationStates.find((item) => item.status === "running");
          setFailedOperation(active?.title ?? "Copy a system partition");
          setFailureMessage("Preview failure. No drive was changed.");
          setOperationStates((current) =>
            current.map((item) =>
              item.status === "running" ? { ...item, status: "failed" } : item,
            ),
          );
          setProgress(null);
          setStep("error");
        }}
      />
    ),
    done: () => (
      <InstallDoneStep
        onFinish={previewMode ? restartWizard : rebootNow}
        preview={previewMode}
        onPreviewError={() => {
          setFailedOperation("Copy a system partition");
          setFailureMessage("Preview failure. No drive was changed.");
          setStep("error");
        }}
      />
    ),
    error: () => (
      <InstallErrorStep
        operation={failedOperation}
        message={failureMessage}
        onBack={() => {
          setSelected(null);
          setEraseConfirmed(false);
          setHealthAccepted(false);
          setStep("drives");
        }}
      />
    ),
  };
  const StepContent = steps[step];

  if (!live) {
    return (
      <>
        <SectionHeader title={copy.install} summary={copy.installSummary} />
        <Column modifiers={[fillMaxWidth()]} horizontalAlignment="start">
          <InstallWelcomeStep
            readiness="unavailable"
            message={copy.installLiveOnly}
            minimumSize={sizeLabel(V1_MINIMUM_DISK_BYTES)}
            onContinue={() => undefined}
            onRefresh={() => undefined}
          />
        </Column>
      </>
    );
  }

  return (
    <>
      <SectionHeader title={copy.install} summary={copy.installSummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <StepContent />
        <ProgressSummary step={step} progress={progress} />
      </Column>
    </>
  );
}
