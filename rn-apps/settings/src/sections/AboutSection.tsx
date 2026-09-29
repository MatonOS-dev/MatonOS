import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import {
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { useMatonSettings } from "../context/MatonSettingsContext";
import { copy, numberFormat } from "../strings";

export function AboutSection() {
  const colors = useMaterialColors();
  const { system } = useMatonSettings();
  const device = system ? `${system.model} (${system.device})` : copy.unknown;
  const sdk = system ? numberFormat.format(system.sdk) : copy.unknown;
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
          <StatusRow label={copy.build} value={system?.build ?? copy.unknown} />
          <StatusRow label={copy.device} value={device} />
          <StatusRow label={copy.sdkLevel} value={sdk} />
        </SettingsCard>
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
