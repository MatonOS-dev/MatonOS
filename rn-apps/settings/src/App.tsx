import { useEffect } from "react";
import { StatusBar } from "expo-status-bar";
import { setBackgroundColorAsync } from "expo-system-ui";
import { Host } from "@expo/ui/jetpack-compose";
import { useColorScheme } from "react-native";
import { MatonSettingsProvider } from "./context/MatonSettingsContext";
import { SettingsShell } from "./components/SettingsShell";

export default function App(): React.JSX.Element {
  const scheme = useColorScheme() ?? "light";
  const background = scheme === "dark" ? "#111318" : "#F8F7FA";
  useEffect(() => {
    void setBackgroundColorAsync(background);
  }, [background]);
  return (
    <>
      <StatusBar style="auto" />
      <Host
        style={{ flex: 1, width: "100%", height: "100%" }}
        colorScheme={scheme}
        useViewportSizeMeasurement
      >
        <MatonSettingsProvider>
          <SettingsShell />
        </MatonSettingsProvider>
      </Host>
    </>
  );
}
