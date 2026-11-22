import { Column } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { SectionHeader } from "../components/SettingsUI";
import { CaptionSettingsSlot } from "../components/CaptionSettingsSlot";
import { copy } from "../strings";

export function CaptionSettingsSection() {
  return (
    <>
      <SectionHeader title={copy.windowTitleBars} summary={copy.windowTitleBarsSummary} />
      <Column modifiers={[fillMaxWidth()]} horizontalAlignment="start">
        <CaptionSettingsSlot style={{ width: "100%", height: 340 }} />
      </Column>
    </>
  );
}
