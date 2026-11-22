import { requireNativeView } from "expo";
import type { ViewProps } from "react-native";

const NativeCaptionSettingsSlot = requireNativeView<ViewProps>(
  "MatonCaptionSettingsSlot",
);

export function CaptionSettingsSlot(props: ViewProps) {
  return <NativeCaptionSettingsSlot {...props} />;
}
