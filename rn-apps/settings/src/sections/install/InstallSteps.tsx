import { useState } from "react";
import { OutlinedTextField, Text, useMaterialColors, useNativeState, type ObservableState } from "@expo/ui/jetpack-compose";
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
import { MatonOS } from "../../bridge/MatonOS";

export type InstallStep =
  | "welcome"
  | "language"
  | "network"
  | "drives"
  | "user"
  | "services"
  | "account"
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
  linux: boolean;
  account?: { username: string; refreshToken: string; machineId: string };
};

// Text lives only in native state, as on the Account page: typing must not re-render React,
// because a re-rendered field drops focus mid-word (seen on live164). Steps read the text and
// validate it when the user leaves the step.
function TextInput({ state, label, secret = false, clean }: { state: ObservableState<string>; label: string; secret?: boolean; clean?: (value: string) => string }) {
  const onValueChange = (next: string) => state.set(clean ? clean(next) : next);
  return (
    <OutlinedTextField value={state} onValueChange={onValueChange} singleLine visualTransformation={secret ? "password" : "none"}>
      <OutlinedTextField.Label><Text>{label}</Text></OutlinedTextField.Label>
    </OutlinedTextField>
  );
}

export function InstallLanguageStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (patch: Partial<SetupDraft>) => void; onBack: () => void; onContinue: () => void }) {
  const locale = useNativeState(draft.locale);
  const timeZone = useNativeState(draft.timeZone);
  const next = () => { onChange({ locale: locale.get(), timeZone: timeZone.get() }); onContinue(); };
  return <SettingsCard title="Language and time">
    <TextInput label="Locale" state={locale} />
    <TextInput label="Time zone" state={timeZone} />
    <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
    <ActionButton label="Continue" onClick={next} />
  </SettingsCard>;
}

export function InstallNetworkStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (patch: Partial<SetupDraft>) => void; onBack: () => void; onContinue: () => void }) {
  const ssid = useNativeState(draft.ssid);
  const password = useNativeState(draft.wifiPassword);
  const [invalid, setInvalid] = useState(false);
  const secured = draft.security !== "open";
  const save = () => onChange({ ssid: ssid.get(), wifiPassword: secured ? password.get() : "" });
  const next = () => {
    const pass = password.get();
    const ok = !ssid.get().trim() || !secured || (pass.length >= 8 && pass.length <= 63);
    setInvalid(!ok);
    if (!ok) return;
    save();
    onContinue();
  };
  const skip = () => { onChange({ ssid: "", wifiPassword: "", hidden: false }); onContinue(); };
  const selectOpen = () => { password.set(""); onChange({ security: "open" }); };
  const passwordField = secured ? <TextInput label="Wi-Fi password" state={password} secret /> : null;
  const passwordError = invalid ? <TextError message="WPA passwords are 8 to 63 characters." /> : null;
  return <SettingsCard title="Wi-Fi (optional)">
    <TextInput label="Network name (SSID)" state={ssid} />
    <ModeRow title="Open" description="No password" selected={draft.security === "open"} onSelect={selectOpen} />
    <ModeRow title="WPA2" description="WPA2 personal" selected={draft.security === "wpa2"} onSelect={() => onChange({ security: "wpa2" })} />
    <ModeRow title="WPA3" description="WPA3 personal" selected={draft.security === "wpa3"} onSelect={() => onChange({ security: "wpa3" })} />
    {passwordField}
    {passwordError}
    <ToggleRow title="Hidden network" supporting="Connect to a network that does not broadcast its name." value={draft.hidden} onChange={(hidden) => onChange({ hidden })} />
    <ActionButton label="Skip Wi-Fi" variant="text" onClick={skip} />
    <ActionButton label={copy.goBack} variant="text" onClick={() => { save(); onBack(); }} />
    <ActionButton label="Continue" onClick={next} />
  </SettingsCard>;
}

export function InstallUserStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (patch: Partial<SetupDraft>) => void; onBack: () => void; onContinue: () => void }) {
  const name = useNativeState(draft.name);
  const value = useNativeState(draft.credentialValue);
  const confirm = useNativeState(draft.credentialConfirm);
  const [invalid, setInvalid] = useState(false);
  const usesCredential = draft.credentialType !== "none";
  const isPin = draft.credentialType === "pin";
  const clean = (text: string) => (isPin ? text.replace(/\D/g, "") : text);
  const save = () => onChange({ name: name.get(), credentialValue: value.get(), credentialConfirm: confirm.get() });
  const next = () => {
    const secret = value.get();
    const validLength = isPin ? /^\d{4,}$/.test(secret) : secret.length >= 4;
    const ok = !usesCredential || (validLength && secret === confirm.get());
    setInvalid(!ok);
    if (!ok) return;
    save();
    onContinue();
  };
  const select = (credentialType: SetupDraft["credentialType"]) => {
    value.set("");
    confirm.set("");
    setInvalid(false);
    onChange({ credentialType, credentialValue: "", credentialConfirm: "" });
  };
  const valueField = usesCredential ? <TextInput label={isPin ? "PIN" : "Password"} state={value} clean={clean} secret /> : null;
  const confirmField = usesCredential ? <TextInput label="Confirm" state={confirm} clean={clean} secret /> : null;
  const credentialError = invalid ? <TextError message="Enter a valid matching credential." /> : null;
  return <SettingsCard title="User and screen lock">
    <TextInput label="Name (optional)" state={name} />
    <ModeRow title="No lock" description="Do not set a screen lock" selected={draft.credentialType === "none"} onSelect={() => select("none")} />
    <ModeRow title="PIN" description="At least 4 digits" selected={isPin} onSelect={() => select("pin")} />
    <ModeRow title="Password" description="At least 4 characters" selected={draft.credentialType === "password"} onSelect={() => select("password")} />
    {valueField}
    {confirmField}
    {credentialError}
    <ActionButton label={copy.goBack} variant="text" onClick={() => { save(); onBack(); }} />
    <ActionButton label="Continue" onClick={next} />
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
  setup.services = { linux: draft.linux };
  if (draft.account) setup.account = draft.account;
  return setup;
}

export function InstallAccountStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (patch: Partial<SetupDraft>) => void; onBack: () => void; onContinue: () => void }) {
  const username = useNativeState("");
  const password = useNativeState("");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const skip = () => { onChange({ account: undefined }); onContinue(); };
  const signIn = async () => {
    setBusy(true); setMessage("");
    try {
      const info = await MatonOS.accountMachineInfo();
      const response = await fetch("https://maton.hanro50.net.za/api/account/login", {
        method: "POST",
        headers: { "content-type": "application/json", "X-Maton-Machine-Id": info.machineId },
        body: JSON.stringify({ username: username.get(), password: password.get(), machine_id: info.machineId, device_name: info.deviceName }),
      });
      const result = await response.json().catch(() => ({})) as Record<string, unknown>;
      if (!response.ok) throw new Error(typeof result.message === "string" ? result.message : `Sign-in failed (${response.status})`);
      if (typeof result.username !== "string" || typeof result.refresh_token !== "string") throw new Error("The account service returned no refresh token.");
      onChange({ account: { username: result.username, refreshToken: result.refresh_token, machineId: info.machineId } });
      password.set("");
      onContinue();
    } catch (error) { setMessage(error instanceof Error ? error.message : "The account request failed."); }
    finally { setBusy(false); }
  };
  const saved = draft.account ? <Text>Signed in as {draft.account.username}.</Text> : null;
  return <SettingsCard title="MatonOS account">
    <Text>Optional. Sign in to add this PC to your MatonOS account. Internet access is required.</Text>
    {saved}
    <TextInput label="Username" state={username} />
    <TextInput label="Password" state={password} secret />
    {message ? <TextError message={message} /> : null}
    <ActionButton label={copy.goBack} variant="text" onClick={onBack} />
    <ActionButton label="Skip" variant="text" onClick={skip} />
    <ActionButton label={busy ? "Signing in…" : "Sign in and continue"} enabled={!busy} onClick={() => void signIn()} />
  </SettingsCard>;
}

export function InstallServicesStep({ draft, onChange, onBack, onContinue }: { draft: SetupDraft; onChange: (patch: Partial<SetupDraft>) => void; onBack: () => void; onContinue: () => void }) {
  const save = () => onChange({ linux: draft.linux });
  const next = () => { save(); onContinue(); };
  const back = () => { save(); onBack(); };
  return <SettingsCard title="Preinstalled services">
    <ToggleRow title="Linux subsystem" supporting="Run Linux desktop apps. You can turn this off later." value={draft.linux} onChange={(linux) => onChange({ linux })} />
    <ActionButton label={copy.goBack} variant="text" onClick={back} />
    <ActionButton label="Continue" onClick={next} />
  </SettingsCard>;
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
  setup,
}: {
  drive: InstallDrive | null;
  setup: SetupDraft;
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
  const summary = setupSummary(setup);
  return (
    <SettingsCard title={cardTitle}>
      <Text style={{ typography: "bodySmall" }}>{summary}</Text>
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

function setupSummary(draft: SetupDraft): string {
  const wifi = draft.ssid.trim() ? draft.ssid.trim() : "not set";
  const lockNames = { none: "none", pin: "PIN set", password: "password set" };
  const name = draft.name.trim() ? draft.name.trim() : "not set";
  return `Language: ${draft.locale || "not set"}\nTime zone: ${draft.timeZone || "not set"}\nWi-Fi: ${wifi}\nName: ${name}\nScreen lock: ${lockNames[draft.credentialType]}\nLinux subsystem: ${draft.linux ? "on" : "off"}`;
}
