import { useEffect, useState } from "react";
import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth, paddingAll } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import { ActionButton, SectionHeader, SettingsCard, ToggleRow } from "../components/SettingsUI";
import { copy } from "../strings";

export function GappsSection() {
  const colors = useMaterialColors();
  const [acknowledged, setAcknowledged] = useState(false);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState(copy.gappsIdle);
  const [deviceId, setDeviceId] = useState("");

  useEffect(() => {
    let active = true;
    void MatonOS.getGappsDeviceId().then((value) => {
      if (active) setDeviceId(value);
    }).catch(() => {
      if (active) setDeviceId("");
    });
    return () => { active = false; };
  }, []);

  const chooseAndInstall = async () => {
    setBusy(true);
    setMessage(copy.gappsBusy);
    try {
      const result = await MatonOS.pickAndInstallGapps();
      if (result.cancelled) setMessage(copy.gappsCancelled);
      else setMessage(result.message);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  };

  const uninstallUpdates = async () => {
    setBusy(true);
    try {
      const result = await MatonOS.uninstallGappsUpdates();
      setMessage(result.message);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  };

  const controls = acknowledged ? (
    <>
      <ActionButton label={copy.gappsSelect} onClick={() => void chooseAndInstall()} enabled={!busy} />
      <ActionButton label={copy.gappsUninstall} onClick={() => void uninstallUpdates()} enabled={!busy} variant="text" />
    </>
  ) : null;
  const idValue = deviceId || "Unavailable";

  return (
    <Column modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 20 }} horizontalAlignment="start">
      <Text style={{ typography: "headlineSmall" }} color={colors.error} modifiers={[fillMaxWidth(), paddingAll(4)]}>
        {copy.gappsWarning}
      </Text>
      <ToggleRow title={copy.gappsAcknowledge} supporting="Required before selecting files or removing updates." value={acknowledged} onChange={setAcknowledged} />
      <SettingsCard title={copy.gapps}>
        <SectionHeader title={copy.gapps} summary={copy.gappsSummary} />
        {controls}
        <Text style={{ typography: "bodyMedium" }} color={colors.onSurfaceVariant}>{message}</Text>
      </SettingsCard>
      <SettingsCard title={copy.gappsDeviceId}>
        <Text style={{ typography: "bodyLarge" }} color={colors.onSurface} selectable>
          {idValue}
        </Text>
        <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
          {copy.gappsDeviceIdHelp}
        </Text>
      </SettingsCard>
    </Column>
  );
}
