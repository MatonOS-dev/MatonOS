import { useLocalSearchParams } from "expo-router";
import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { EmbeddedSettingsSlot } from "../components/EmbeddedSettingsSlot";
import { SectionHeader } from "../components/SettingsUI";
import { copy } from "../strings";

export function EmbeddedAppSettingsSection() {
  const colors = useMaterialColors();
  const params = useLocalSearchParams<{ component?: string | string[]; label?: string | string[] }>();
  const component = Array.isArray(params.component) ? params.component[0] : params.component;
  const label = Array.isArray(params.label) ? params.label[0] : params.label;
  if (!component) return <Text color={colors.error}>{copy.appSettingsProviderMissing}</Text>;
  return (
    <Column modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 12 }} horizontalAlignment="start">
      <SectionHeader title={label || copy.appSettings} summary={component} />
      <EmbeddedSettingsSlot componentName={component} style={{ width: "100%", height: 340 }} />
    </Column>
  );
}
