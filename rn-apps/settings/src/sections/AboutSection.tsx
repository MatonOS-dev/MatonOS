import { Column, Text, TextButton, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import {
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { useMatonSettings } from "../context/MatonSettingsContext";
import { copy, numberFormat } from "../strings";
import { useDeveloperOptions } from "../context/DeveloperOptionsContext";
import { useState } from "react";

export function AboutSection() {
  const colors = useMaterialColors();
  const { system } = useMatonSettings();
  const { enabled, enable } = useDeveloperOptions();
  const [tapCount, setTapCount] = useState(0);
  const [hint, setHint] = useState("");
  const device = system ? `${system.model} (${system.device})` : copy.unknown;
  const sdk = system ? numberFormat.format(system.sdk) : copy.unknown;
  const onBuildTap = () => {
    if (enabled) {
      setHint(copy.developerAlreadyEnabled);
      return;
    }
    const next = tapCount + 1;
    setTapCount(next);
    if (next >= 5) {
      void enable().then(() => setHint(copy.developerEnabled)).catch((error: unknown) => {
        console.error("Could not enable developer options", error);
        setHint(copy.developerEnableFailed);
      });
      return;
    }
    setHint(copy.developerTapsRemaining(next));
  };
  return (
    <>
      <SectionHeader title={copy.about} summary={copy.aboutSummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.app}>
          <StatusRow
            label={copy.osVersion}
            value={system?.version ?? copy.unknown}
          />
          <TextButton onClick={onBuildTap}>
            <StatusRow label={copy.build} value={system?.build ?? copy.unknown} />
          </TextButton>
          <StatusRow label={copy.device} value={device} />
          <StatusRow label={copy.sdkLevel} value={sdk} />
        </SettingsCard>
        {hint ? <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>{hint}</Text> : null}
        <Text
          style={{ typography: "bodyMedium" }}
          color={colors.onSurfaceVariant}
        >
          {copy.basedOnAosp}
        </Text>
      </Column>
    </>
  );
}
