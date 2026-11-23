import { useCallback, useState } from "react";
import { useFocusEffect, useRouter } from "expo-router";
import { Column, Icon, ListItem, Text, TextButton, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth, paddingAll } from "@expo/ui/jetpack-compose/modifiers";
import { copy } from "../strings";
import { SectionHeader } from "../components/SettingsUI";
import { EmbeddedSettings, type EmbeddedSettingsEntry } from "../appsettings/EmbeddedSettings";

export function AppSettingsSection() {
  const colors = useMaterialColors();
  const router = useRouter();
  const [entries, setEntries] = useState<EmbeddedSettingsEntry[]>([]);
  const [error, setError] = useState("");

  const refresh = useCallback(async () => {
    try {
      setEntries(await EmbeddedSettings.list());
      setError("");
    } catch (reason) {
      console.error("embedded app settings discovery failed", reason);
      setError(`${copy.appSettingsUnavailable}: ${reason instanceof Error ? reason.message : String(reason)}`);
    }
  }, []);

  useFocusEffect(useCallback(() => { void refresh(); }, [refresh]));

  const rows = entries.map((entry) => (
    <TextButton
      key={entry.component}
      onClick={() => router.push({ pathname: "/app-settings/detail", params: { component: entry.component, label: entry.label } })}
      colors={{ contentColor: colors.onSurface }}
      modifiers={[fillMaxWidth()]}
    >
      <ListItem modifiers={[fillMaxWidth(), paddingAll(4)]}>
        <ListItem.LeadingContent>
          {entry.icon ? <Icon source={{ uri: entry.icon }} size={36} contentDescription={entry.label} /> : null}
        </ListItem.LeadingContent>
        <ListItem.HeadlineContent>
          <Text style={{ typography: "bodyLarge" }} color={colors.onSurface}>{entry.label}</Text>
        </ListItem.HeadlineContent>
        <ListItem.TrailingContent>
          <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>{entry.package}</Text>
        </ListItem.TrailingContent>
      </ListItem>
    </TextButton>
  ));

  return (
    <Column modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 8 }} horizontalAlignment="start">
      <SectionHeader title={copy.appSettings} summary={copy.appSettingsSummary} />
      {error ? <Text color={colors.error}>{error}</Text> : null}
      {rows}
      {!error && entries.length === 0 ? <Text color={colors.onSurfaceVariant}>{copy.noEmbeddedAppSettings}</Text> : null}
    </Column>
  );
}
