import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  SectionHeader,
  SettingsCard,
} from "../components/SettingsUI";
import { copy } from "../strings";

export function DeveloperSection() {
  const colors = useMaterialColors();
  return (
    <>
      <SectionHeader title={copy.developer} summary={copy.developerSummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.trustedApps}>
          <Text style={{ typography: "bodyLarge" }} color={colors.onSurface}>
            {copy.trustedAppsDescription}
          </Text>
          <Text
            style={{ typography: "bodyMedium" }}
            color={colors.onSurfaceVariant}
          >
            {copy.trustedAppsDetails}
          </Text>
          <ActionButton
            label={copy.openTrustedApps}
            onClick={() => void MatonOS.openTrustedApps()}
          />
        </SettingsCard>
      </Column>
    </>
  );
}
