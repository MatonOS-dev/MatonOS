import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import {
  Column,
  LinearProgressIndicator,
  Text,
  useMaterialColors,
} from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { useMatonSettings } from "../context/MatonSettingsContext";
import { copy } from "../strings";
import {
  checkForUpdate,
  installUpdate,
  readUpdateStatus,
  type UpdateCheck,
  type UpdateProgress,
  type UpdateStatus,
} from "../updater/UpdateService";

function offeredMessage(result: UpdateCheck): string {
  if (result.kind === "up-to-date") return copy.updateUpToDate;
  if (result.kind === "available")
    return copy.updateAvailable.replace("{version}", result.version);
  const reasons: Record<string, string> = {
    missing_addons: copy.updateReasonMissingAddons,
    android_base_too_old: copy.updateReasonAndroidBase,
  };
  return (
    reasons[result.reason] ?? `${copy.updateNotOffered} (${result.reason})`
  );
}

function UpdateStatusCard({ status }: { status: UpdateStatus | null }) {
  if (!status) return null;
  return (
    <SettingsCard title={copy.updateStatus}>
      <StatusRow
        label={copy.stagedUpdate}
        value={status.available ? copy.available : copy.unavailable}
      />
      <StatusRow label={copy.installedSlot} value={status.slot} />
    </SettingsCard>
  );
}

function UpdateProgressCard({ progress }: { progress: UpdateProgress }) {
  const colors = useMaterialColors();
  const file = progress.file ? ` — ${progress.file}` : "";
  const amount =
    progress.bytesTotal > 0 ? progress.bytesDone / progress.bytesTotal : null;
  return (
    <SettingsCard title={copy.updateInProgress}>
      <Text
        style={{ typography: "bodyMedium" }}
        color={colors.onSurfaceVariant}
      >
        {`${progress.step}${file}`}
      </Text>
      <LinearProgressIndicator progress={amount} />
    </SettingsCard>
  );
}

export function SystemUpdateSection() {
  const colors = useMaterialColors();
  const { system } = useMatonSettings();
  const [status, setStatus] = useState<UpdateStatus | null>(null);
  const [statusError, setStatusError] = useState("");
  const [result, setResult] = useState<UpdateCheck | null>(null);
  const [progress, setProgress] = useState<UpdateProgress | null>(null);
  const [failure, setFailure] = useState("");
  const [busy, setBusy] = useState(false);
  const [restartReady, setRestartReady] = useState(false);

  const refreshStatus = useCallback(async () => {
    setStatusError("");
    try {
      setStatus(await readUpdateStatus());
    } catch (error) {
      setStatus(null);
      setStatusError(
        error instanceof Error ? error.message : copy.updateStatusUnavailable,
      );
    }
  }, []);

  useFocusEffect(
    useCallback(() => {
      void refreshStatus();
    }, [refreshStatus]),
  );

  const check = async () => {
    setBusy(true);
    setFailure("");
    setResult(null);
    try {
      if (!system?.version) throw new Error(copy.updateNoInstalledVersion);
      setResult(await checkForUpdate(system.version));
    } catch (error) {
      setFailure(error instanceof Error ? error.message : copy.updateChecking);
    } finally {
      setBusy(false);
    }
  };

  const install = async () => {
    if (result?.kind !== "available") return;
    setBusy(true);
    setFailure("");
    setRestartReady(false);
    try {
      await installUpdate(result, setProgress);
      setProgress(null);
      setRestartReady(true);
      await refreshStatus();
    } catch (error) {
      setProgress(null);
      setFailure(error instanceof Error ? error.message : copy.installFailed);
    } finally {
      setBusy(false);
    }
  };

  const restart = async () => {
    setBusy(true);
    setFailure("");
    try {
      const restarted = await MatonOS.reboot();
      if (!restarted) throw new Error(copy.updateRestartFailed);
    } catch (error) {
      setFailure(
        `Restart: ${error instanceof Error ? error.message : copy.updateRestartFailed}`,
      );
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <SectionHeader
        title={copy.systemUpdate}
        summary={copy.systemUpdateSummary}
      />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.systemUpdate}>
          <StatusRow
            label={copy.osVersion}
            value={system?.version ?? copy.unknown}
          />
          <StatusRow
            label={copy.installedChannel}
            value={copy.updateChannelMain}
          />
          <ActionButton
            label={copy.checkForUpdates}
            onClick={() => void check()}
            enabled={!busy}
          />
          {result ? (
            <Text
              style={{ typography: "bodyMedium" }}
              color={colors.onSurfaceVariant}
            >
              {offeredMessage(result)}
            </Text>
          ) : null}
          {result?.kind === "available" ? (
            <ActionButton
              label={copy.downloadAndInstall}
              onClick={() => void install()}
              enabled={!busy}
            />
          ) : null}
          {restartReady ? (
            <Column
              verticalArrangement={{ spacedBy: 8 }}
              horizontalAlignment="start"
            >
              <Text
                style={{ typography: "bodyMedium" }}
                color={colors.onSurfaceVariant}
              >
                {copy.updateRestartRequired}
              </Text>
              <ActionButton
                label={copy.restartNow}
                onClick={() => void restart()}
                enabled={!busy}
              />
            </Column>
          ) : null}
          {failure ? (
            <Text style={{ typography: "bodyMedium" }} color={colors.error}>
              {failure}
            </Text>
          ) : null}
        </SettingsCard>
        <UpdateStatusCard status={status} />
        {statusError ? (
          <SettingsCard title={copy.updateStatus}>
            <Text style={{ typography: "bodyMedium" }} color={colors.error}>
              {statusError}
            </Text>
            <ActionButton
              label={copy.refreshStatus}
              onClick={() => void refreshStatus()}
              enabled={!busy}
            />
          </SettingsCard>
        ) : null}
        {progress ? <UpdateProgressCard progress={progress} /> : null}
      </Column>
    </>
  );
}
