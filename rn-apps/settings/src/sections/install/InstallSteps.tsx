import { OutlinedTextField, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import {
  ActionButton,
  ModeRow,
  SettingsCard,
  StatusRow,
  ToggleRow,
} from "../../components/SettingsUI";
import type { DriveHealth, InstallDrive, OperationProgress, SetupV1 } from "../../installer/InstallerApi";
import { createV1Plan } from "../../installer/createV1Plan";
import { copy } from "../../strings";

export type InstallStep =
  | "welcome"
  | "language"
  | "network"
  | "drives"
  | "user"
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

export type SetupDraft = {
  locale: string;
  timeZone: string;
  ssid: string;
  security: "open" | "wpa2" | "wpa3";
  wifiPassword: string;
  hidden: boolean;
  name: string;
  credentialType: "none" | "pin" | "password";
  credentialValue: string;
  credentialConfirm: string;
};

function TextInput({ label, value, onChange, secret = false }: { label: string; value: string; onChange: (value: string) => void; secret?: boolean }) {
  return (
    <OutlinedTextField value={value} onValueChange={onChange} singleLine visualTransformation={secret ? "password" : "none"}>
      <OutlinedTextField.Label><Text>{label}</Text></OutlinedTextField.Label>
    </OutlinedTextField>
  );
}

export function InstallLanguageStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (draft: SetupDraft) => void; onBack: () => void; onContinue: () => void }) {
  return <SettingsCard title="Language and time">
    <TextInput label="Locale" value={draft.locale} onChange={(locale) => onChange({ ...draft, locale })} />
    <TextInput label="Time zone" value={draft.timeZone} onChange={(timeZone) => onChange({ ...draft, timeZone })} />
    <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
    <ActionButton label="Continue" onClick={onContinue} />
  </SettingsCard>;
}

export function InstallNetworkStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (draft: SetupDraft) => void; onBack: () => void; onContinue: () => void }) {
  const secured = draft.security !== "open";
  return <SettingsCard title="Wi-Fi (optional)">
    <TextInput label="Network name (SSID)" value={draft.ssid} onChange={(ssid) => onChange({ ...draft, ssid })} />
    <ModeRow title="Open" description="No password" selected={draft.security === "open"} onSelect={() => onChange({ ...draft, security: "open", wifiPassword: "" })} />
    <ModeRow title="WPA2" description="WPA2 personal" selected={draft.security === "wpa2"} onSelect={() => onChange({ ...draft, security: "wpa2" })} />
    <ModeRow title="WPA3" description="WPA3 personal" selected={draft.security === "wpa3"} onSelect={() => onChange({ ...draft, security: "wpa3" })} />
    {secured ? <TextInput label="Wi-Fi password" value={draft.wifiPassword} onChange={(wifiPassword) => onChange({ ...draft, wifiPassword })} secret /> : null}
    <ToggleRow title="Hidden network" supporting="Connect to a network that does not broadcast its name." value={draft.hidden} onChange={(hidden) => onChange({ ...draft, hidden })} />
    <ActionButton label="Skip Wi-Fi" variant="text" onClick={() => { onChange({ ...draft, ssid: "", wifiPassword: "", hidden: false }); onContinue(); }} />
    <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
    <ActionButton label="Continue" onClick={onContinue} />
  </SettingsCard>;
}

export function InstallUserStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (draft: SetupDraft) => void; onBack: () => void; onContinue: () => void }) {
  const usesCredential = draft.credentialType !== "none";
  const validLength = draft.credentialType === "pin" ? /^\d{4,}$/.test(draft.credentialValue) : draft.credentialValue.length >= 4;
  const valid = !usesCredential || (validLength && draft.credentialValue === draft.credentialConfirm);
  return <SettingsCard title="User and screen lock">
    <TextInput label="Name (optional)" value={draft.name} onChange={(name) => onChange({ ...draft, name })} />
    <ModeRow title="No lock" description="Do not set a screen lock" selected={draft.credentialType === "none"} onSelect={() => onChange({ ...draft, credentialType: "none", credentialValue: "", credentialConfirm: "" })} />
    <ModeRow title="PIN" description="At least 4 digits" selected={draft.credentialType === "pin"} onSelect={() => onChange({ ...draft, credentialType: "pin" })} />
    <ModeRow title="Password" description="At least 4 characters" selected={draft.credentialType === "password"} onSelect={() => onChange({ ...draft, credentialType: "password" })} />
    {usesCredential ? <TextInput label={draft.credentialType === "pin" ? "PIN" : "Password"} value={draft.credentialValue} onChange={(credentialValue) => onChange({ ...draft, credentialValue: draft.credentialType === "pin" ? credentialValue.replace(/\D/g, "") : credentialValue })} secret /> : null}
    {usesCredential ? <TextInput label="Confirm" value={draft.credentialConfirm} onChange={(credentialConfirm) => onChange({ ...draft, credentialConfirm: draft.credentialType === "pin" ? credentialConfirm.replace(/\D/g, "") : credentialConfirm })} secret /> : null}
    {usesCredential && !valid ? <TextError message="Enter a valid matching credential." /> : null}
    <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
    <ActionButton label="Continue" enabled={valid} onClick={onContinue} />
  </SettingsCard>;
}

export function makeSetup(draft: SetupDraft): SetupV1 {
  const setup: SetupV1 = { version: 1 };
  if (draft.locale.trim()) setup.locale = draft.locale.trim();
  if (draft.timeZone.trim()) setup.timeZone = draft.timeZone.trim();
  if (draft.ssid.trim()) {
    const wifi: NonNullable<SetupV1["wifi"]>[number] = { ssid: draft.ssid.trim(), security: draft.security };
    if (draft.security !== "open" && draft.wifiPassword) wifi.password = draft.wifiPassword;
    if (draft.hidden) wifi.hidden = true;
    setup.wifi = [wifi];
  }
  const user: NonNullable<SetupV1["user"]> = {};
  if (draft.name.trim()) user.name = draft.name.trim();
  if (draft.credentialType !== "none" && draft.credentialValue) user.credential = { type: draft.credentialType, value: draft.credentialValue };
  if (user.name || user.credential) setup.user = user;
  return setup;
}

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
      <HealthFindings health={drive.health} />
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

export const HEALTH_ACCEPT_TEXT =
  "This drive reports SMART errors. Do you accept that you may experience technical difficulties and file corruption if we install to it?";

export function hasHealthErrors(drive: InstallDrive | null) {
  return !!drive?.health?.findings.some((finding) => finding.level === "error");
}

function healthSummary(health: DriveHealth | undefined) {
  if (!health?.known) return "Health unknown";
  if (health.findings.length === 0) return "No problems reported";
  if (health.findings.some((finding) => finding.level === "error")) return "SMART errors";
  return "Warnings";
}

function HealthFindings({ health }: { health: DriveHealth | undefined }) {
  const colors = useMaterialColors();
  const findings = health?.known ? health.findings : [];
  const rows = findings.map((finding, index) => (
    <Text
      key={`${index}:${finding.message}`}
      style={{ typography: "bodySmall" }}
      color={finding.level === "error" ? colors.error : colors.tertiary}
    >
      {finding.message}
    </Text>
  ));
  return (
    <>
      <StatusRow label="Drive health" value={healthSummary(health)} />
      {rows}
    </>
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
  // Compose host tree: RN FlatList does not render here; build rows up front.
  const data = preview ? drives.slice(0, 1) : drives;
  const rows = data.map((drive) => (
    <DriveCard
      key={drive.id}
      drive={drive}
      selected={drive.id === selectedId}
      onSelect={onSelect}
      formatSize={formatSize}
    />
  ));
  return <>{rows}</>;
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

const PARTITION_PURPOSE: Record<string, string> = {
  esp: "Boot menu and the signed MatonOS boot image.",
  maton_system: "Read-only MatonOS system images.",
  metadata: "Encryption and image extent bookkeeping.",
  userdata: "Your apps, files and settings.",
};

function PartitionSummary({
  drive,
  formatSize,
}: {
  drive: InstallDrive;
  formatSize: (bytes: number) => string;
}) {
  // Same pure plan the install runs, so sizes shown here are the sizes written.
  let partitions: ReturnType<typeof createV1Plan>["partitions"];
  try {
    partitions = createV1Plan(drive.id, drive.identity, drive.sizeBytes, drive.rotational).partitions;
  } catch (problem) {
    const message = problem instanceof Error ? problem.message : String(problem);
    return <TextError message={message} />;
  }
  const rows = partitions.map((part) => (
    <StatusRow
      key={part.name}
      label={part.name}
      value={formatSize(part.sizeBytes)}
      supporting={PARTITION_PURPOSE[part.name] ?? ""}
    />
  ));
  return (
    <>
      <Text style={{ typography: "titleSmall" }}>New partitions on this drive</Text>
      {rows}
    </>
  );
}

function HealthAcceptance({
  drive,
  accepted,
  onAcceptedChange,
}: {
  drive: InstallDrive;
  accepted: boolean;
  onAcceptedChange: (value: boolean) => void;
}) {
  if (!hasHealthErrors(drive)) return null;
  return (
    <ToggleRow
      title={HEALTH_ACCEPT_TEXT}
      supporting="Installation is still allowed. Back up anything important elsewhere."
      value={accepted}
      onChange={onAcceptedChange}
    />
  );
}

function ConfirmBody({
  drive,
  preview,
  confirmed,
  onConfirmChange,
  healthAccepted,
  onHealthAcceptedChange,
  formatSize,
}: {
  drive: InstallDrive | null;
  preview: boolean;
  confirmed: boolean;
  onConfirmChange: (value: boolean) => void;
  healthAccepted: boolean;
  onHealthAcceptedChange: (value: boolean) => void;
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
      <HealthFindings health={drive.health} />
      <PartitionSummary drive={drive} formatSize={formatSize} />
      <HealthAcceptance
        drive={drive}
        accepted={healthAccepted}
        onAcceptedChange={onHealthAcceptedChange}
      />
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
  healthAccepted,
  onHealthAcceptedChange,
  onBack,
  onStart,
  formatSize,
}: {
  drive: InstallDrive | null;
  confirmed: boolean;
  preview: boolean;
  onConfirmChange: (value: boolean) => void;
  healthAccepted: boolean;
  onHealthAcceptedChange: (value: boolean) => void;
  onBack: () => void;
  onStart: () => void;
  formatSize: (bytes: number) => string;
}) {
  const cardTitle = preview ? "Preview confirmation" : "Confirm installation";
  const confirmLabel = preview ? "Start preview" : copy.confirmInstall;
  const healthOk = !hasHealthErrors(drive) || healthAccepted;
  return (
    <SettingsCard title={cardTitle}>
      <ConfirmBody
        drive={drive}
        preview={preview}
        confirmed={confirmed}
        onConfirmChange={onConfirmChange}
        healthAccepted={healthAccepted}
        onHealthAcceptedChange={onHealthAcceptedChange}
        formatSize={formatSize}
      />
      <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
      <ActionButton
        label={confirmLabel}
        enabled={!!drive && drive.safe && confirmed && healthOk}
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
  const rows = data.map((operation, index) => (
    <OperationRow key={`${index}:${operation.title}`} operation={operation} />
  ));
  return <>{rows}</>;
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
    : "Every install operation completed successfully. Remove the live USB or other live media, then reboot to start the installed system.";
  const finishLabel = preview ? "Start again" : "Reboot";
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
      <ActionButton label={finishLabel} onClick={onFinish} />
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
