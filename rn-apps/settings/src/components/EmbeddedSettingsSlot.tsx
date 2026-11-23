import { requireNativeView } from "expo";
import type { ViewProps } from "react-native";

type EmbeddedSettingsSlotProps = ViewProps & { componentName: string };

const NativeEmbeddedSettingsSlot = requireNativeView<EmbeddedSettingsSlotProps>(
  "MatonEmbeddedSettingsSlot",
);

export function EmbeddedSettingsSlot(props: EmbeddedSettingsSlotProps) {
  return <NativeEmbeddedSettingsSlot {...props} />;
}
