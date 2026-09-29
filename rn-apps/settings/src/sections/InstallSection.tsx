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
import type {
  InstallDrive,
  InstallerOperation,
  OperationProgress,
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
  return `${numberFormat.format(bytes / 1024 ** 3)} GiB`;
}

function parse<T>(value: string): T | null {
  try {
    return JSON.parse(value) as T;
  } catch {
    return null;
  }
}

function operationTitle(operation: InstallerOperation) {
  const labels: Record<InstallerOperation["kind"], string> = {
    write_gpt: "Write the target partition table",
    create_lp_metadata: "Create dynamic partition metadata",
    format: "Format a target partition",
    copy_partition: "Copy a system partition",
    clone_partition: "Clone a system partition",
    write_files: "Write boot and loader files",
  };
  return labels[operation.kind];
}

export function InstallSection() {
  const colors = useMaterialColors();
  const { live } = useMatonSettings();
  const [step, setStep] = useState<InstallStep>("welcome");
  const [drives, setDrives] = useState<InstallDrive[]>([]);
  const [selected, setSelected] = useState<InstallDrive | null>(null);
  const [eraseConfirmed, setEraseConfirmed] = useState(false);
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
        { title: "Create dynamic partition metadata", status: "complete", progress: null },
        { title: "Copy a system partition", status: "running", progress: null },
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

  const restartWizard = () => {
    previewModeRef.current = false;
    setSelected(null);
    setEraseConfirmed(false);
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
        onFinish={restartWizard}
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
        {step === "progress" && progress ? (
          <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
            {progress.message}
          </Text>
        ) : null}
        {step === "progress" && progress ? (
          <LinearProgressIndicator
            progress={
              progress.bytesTotal > 0
                ? progress.bytesDone / progress.bytesTotal
                : null
            }
          />
        ) : null}
      </Column>
    </>
  );
}
