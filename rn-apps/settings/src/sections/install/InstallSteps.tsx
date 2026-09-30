import { Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { FlatList } from "react-native";
import {
  ActionButton,
  SettingsCard,
  StatusRow,
  ToggleRow,
} from "../../components/SettingsUI";
import type { InstallDrive, OperationProgress } from "../../installer/InstallerApi";
import { copy } from "../../strings";

export type InstallStep =
  | "welcome"
  | "drives"
  | "confirm"
  | "progress"
  | "done"
  | "error";

export type OperationState = {
  title: string;
  status: "waiting" | "running" | "complete" | "failed";
  progress: OperationProgress | null;
};

export type InstallReadiness = "loading" | "ready" | "unavailable";

function DriveCard({
  drive,
  selected,
  onSelect,
  formatSize,
}: {
  drive: InstallDrive;
  selected: boolean;
  onSelect: (drive: InstallDrive) => void;
  formatSize: (bytes: number) => string;
}) {
  const transport = drive.removable
    ? `${drive.transport} · removable`
    : drive.transport;
  return (
    <SettingsCard title={drive.model}>
      <StatusRow label={copy.capacity} value={formatSize(drive.sizeBytes)} />
      <StatusRow label={copy.connection} value={transport} />
      <StatusRow label={copy.device} value={drive.path} />
      <ActionButton
        label={selected ? copy.selected : copy.selectDrive}
        enabled={drive.safe}
        onClick={() => onSelect(drive)}
      />
      {!drive.safe ? (
        <TextError message={drive.reason || "This drive is not a safe target."} />
      ) : null}
    </SettingsCard>
  );
}

function TextError({ message }: { message: string }) {
  const colors = useMaterialColors();
  return (
    <Text style={{ typography: "bodySmall" }} color={colors.error}>
      {message}
    </Text>
  );
}

function ReadinessControl({
  readiness,
  onContinue,
  onRefresh,
}: {
  readiness: InstallReadiness;
  onContinue: () => void;
  onRefresh: () => void;
}) {
  if (readiness === "loading") {
    return (
      <Text style={{ typography: "bodySmall" }}>
        Checking installer and available drives…
      </Text>
    );
  }
  if (readiness === "ready") {
    return <ActionButton label="Choose a drive" onClick={onContinue} />;
  }
  if (readiness === "unavailable") {
    return (
      <ActionButton label={copy.refresh} variant="text" onClick={onRefresh} />
    );
  }
  return null;
}

export function InstallWelcomeStep({
  readiness,
  message,
  minimumSize,
  onContinue,
  onRefresh,
  onPreview,
}: {
  readiness: InstallReadiness;
  message: string;
  minimumSize: string;
  onContinue: () => void;
  onRefresh: () => void;
  onPreview?: () => void;
}) {
  return (
    <SettingsCard title={copy.installProfileHeading}>
      <Text style={{ typography: "bodyMedium" }}>
        Install MatonOS on a separate drive. The selected drive will be erased.
        The live system remains available until you reboot.
      </Text>
      <StatusRow label={copy.minimumDrive} value={minimumSize} />
      <StatusRow
        label={copy.slotPlan}
        value={copy.slotLabels}
        supporting={copy.slotDescription}
      />
      <Text style={{ typography: "bodySmall" }}>
        User files are not copied. The installed system starts with empty user
        storage.
      </Text>
      {message ? <TextError message={message} /> : null}
      <ReadinessControl
        readiness={readiness}
        onContinue={onContinue}
        onRefresh={onRefresh}
      />
      {__DEV__ && onPreview ? (
        <ActionButton
          label="Preview screens (no install)"
          variant="text"
          onClick={onPreview}
        />
      ) : null}
    </SettingsCard>
  );
}

function DrivesStatus({
  ready,
  error,
  count,
}: {
  ready: boolean;
  error: string;
  count: number;
}) {
  if (!ready && !error) {
    return (
      <Text style={{ typography: "bodySmall" }}>Loading drive list…</Text>
    );
  }
  if (error) {
    return <TextError message={error} />;
  }
  if (ready && count === 0) {
    return (
      <Text style={{ typography: "bodyMedium" }}>{copy.noSafeDrives}</Text>
    );
  }
  return null;
}

function DriveList({
  drives,
  preview,
  selectedId,
  onSelect,
  formatSize,
}: {
  drives: InstallDrive[];
  preview: boolean;
  selectedId: string;
  onSelect: (drive: InstallDrive) => void;
  formatSize: (bytes: number) => string;
}) {
  const data = preview ? drives.slice(0, 1) : drives;
  return (
    <FlatList
      style={{ height: 320 }}
      data={data}
      keyExtractor={(drive) => drive.id}
      renderItem={({ item }) => (
        <DriveCard
          drive={item}
          selected={item.id === selectedId}
          onSelect={onSelect}
          formatSize={formatSize}
        />
      )}
    />
  );
}

export function InstallDrivesStep({
  drives,
  preview,
  error,
  ready,
  selectedId,
  onSelect,
  onRefresh,
  formatSize,
}: {
  drives: InstallDrive[];
  preview: boolean;
  error: string;
  ready: boolean;
  selectedId: string;
  onSelect: (drive: InstallDrive) => void;
  onRefresh: () => void;
  formatSize: (bytes: number) => string;
}) {
  return (
    <>
      <SettingsCard title="Choose a target drive">
        <Text style={{ typography: "bodyMedium" }}>
          Select the drive to install to. MatonOS will erase every partition
          and file on that drive.
        </Text>
        <DrivesStatus ready={ready} error={error} count={drives.length} />
        <ActionButton label={copy.refresh} variant="text" onClick={onRefresh} />
      </SettingsCard>
      <DriveList
        drives={drives}
        preview={preview}
        selectedId={selectedId}
        onSelect={onSelect}
        formatSize={formatSize}
      />
    </>
  );
}

function ConfirmBody({
  drive,
  preview,
  confirmed,
  onConfirmChange,
  formatSize,
}: {
  drive: InstallDrive | null;
  preview: boolean;
  confirmed: boolean;
  onConfirmChange: (value: boolean) => void;
  formatSize: (bytes: number) => string;
}) {
  const colors = useMaterialColors();
  if (!drive) {
    return <TextError message="No target drive is selected." />;
  }
  const eraseText = `This will erase ${drive.model} (${formatSize(drive.sizeBytes)}) and all data on it.`;
  return (
    <>
      {preview ? (
        <Text style={{ typography: "bodyMedium" }} color={colors.primary}>
          Preview only. No drive will be changed.
        </Text>
      ) : null}
      <Text style={{ typography: "titleMedium" }} color={colors.error}>
        {eraseText}
      </Text>
      <StatusRow label={copy.device} value={drive.path} />
      <StatusRow label={copy.capacity} value={formatSize(drive.sizeBytes)} />
      <ToggleRow
        title="I understand this drive will be erased"
        supporting="This confirmation is required before installation starts."
        value={confirmed}
        onChange={onConfirmChange}
      />
    </>
  );
}

export function InstallConfirmStep({
  drive,
  confirmed,
  preview,
  onConfirmChange,
  onBack,
  onStart,
  formatSize,
}: {
  drive: InstallDrive | null;
  confirmed: boolean;
  preview: boolean;
  onConfirmChange: (value: boolean) => void;
  onBack: () => void;
  onStart: () => void;
  formatSize: (bytes: number) => string;
}) {
  const cardTitle = preview ? "Preview confirmation" : "Confirm installation";
  const confirmLabel = preview ? "Start preview" : copy.confirmInstall;
  return (
    <SettingsCard title={cardTitle}>
      <ConfirmBody
        drive={drive}
        preview={preview}
        confirmed={confirmed}
        onConfirmChange={onConfirmChange}
        formatSize={formatSize}
      />
      <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
      <ActionButton
        label={confirmLabel}
        enabled={!!drive && drive.safe && confirmed}
        onClick={onStart}
      />
    </SettingsCard>
  );
}

function OperationRow({ operation }: { operation: OperationState }) {
  const labels: Record<OperationState["status"], string> = {
    waiting: "Waiting",
    running: "In progress",
    complete: "Complete",
    failed: "Failed",
  };
  const status = labels[operation.status];
  const supporting = operation.progress?.message || status;
  return (
    <StatusRow
      label={operation.title}
      value={status}
      supporting={supporting}
    />
  );
}

function OperationList({
  operations,
  preview,
}: {
  operations: OperationState[];
  preview: boolean;
}) {
  const data = preview ? operations.slice(0, 4) : operations;
  return (
    <FlatList
      style={{ height: 240 }}
      data={data}
      keyExtractor={(operation, index) => `${index}:${operation.title}`}
      renderItem={({ item }) => <OperationRow operation={item} />}
    />
  );
}

export function InstallProgressStep({
  operations,
  progress,
  count,
  preview,
  onPreviewComplete,
  onPreviewFailure,
}: {
  operations: OperationState[];
  progress: OperationProgress | null;
  count: number;
  preview: boolean;
  onPreviewComplete: () => void;
  onPreviewFailure: () => void;
}) {
  return (
    <SettingsCard title="Installing MatonOS">
      <Text style={{ typography: "bodyMedium" }}>
        {progress?.message || "Preparing installation…"}
      </Text>
      <Text style={{ typography: "bodySmall" }}>
        {operations.filter((item) => item.status === "complete").length} of {count} operations complete
      </Text>
      <OperationList operations={operations} preview={preview} />
      {preview ? (
        <>
          <ActionButton
            label="Preview done screen"
            variant="text"
            onClick={onPreviewComplete}
          />
          <ActionButton
            label="Preview error screen"
            variant="text"
            onClick={onPreviewFailure}
          />
        </>
      ) : null}
    </SettingsCard>
  );
}

export function InstallDoneStep({
  onFinish,
  preview,
  onPreviewError,
}: {
  onFinish: () => void;
  preview: boolean;
  onPreviewError: () => void;
}) {
  const title = preview ? "Preview complete" : "Installation complete";
  const message = preview
    ? "This preview finished without calling the installer. No drive was changed."
    : "Every install operation completed successfully. Reboot from the system power menu and remove the live USB or other live media to start the installed system.";
  return (
    <SettingsCard title={title}>
      <Text style={{ typography: "bodyMedium" }}>
        {message}
      </Text>
      {preview ? (
        <ActionButton
          label="Preview error screen"
          variant="text"
          onClick={onPreviewError}
        />
      ) : null}
      <ActionButton label="Install another drive" variant="text" onClick={onFinish} />
    </SettingsCard>
  );
}

export function InstallErrorStep({
  operation,
  message,
  onBack,
}: {
  operation: string;
  message: string;
  onBack: () => void;
}) {
  const colors = useMaterialColors();
  return (
    <SettingsCard title="Installation failed">
      <Text style={{ typography: "bodyMedium" }} color={colors.error}>
        {operation}
      </Text>
      <TextError message={message} />
      <Text style={{ typography: "bodySmall" }}>
        Review the target before trying again. The drive may have been partly
        changed by completed operations.
      </Text>
      <ActionButton label="Choose a drive" variant="text" onClick={onBack} />
    </SettingsCard>
  );
}
