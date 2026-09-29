import { Slot } from "expo-router";
import { Column, Row } from "@expo/ui/jetpack-compose";
import {
  fillMaxHeight,
  fillMaxSize,
  paddingAll,
  verticalScroll,
  weight,
  width,
} from "@expo/ui/jetpack-compose/modifiers";
import { NavRail, BackButton } from "./SettingsUI";
import { useMatonSettings } from "../context/MatonSettingsContext";

export function SettingsShell() {
  const { live } = useMatonSettings();
  return (
    <Row
      modifiers={[fillMaxSize()]}
      verticalAlignment="top"
      horizontalArrangement="start"
    >
      <NavRail live={live} />
      <Column
        modifiers={[weight(1), fillMaxHeight(), paddingAll(28)]}
        verticalArrangement="top"
        horizontalAlignment="start"
      >
        <Column
          modifiers={[width(820), fillMaxHeight(), verticalScroll()]}
          verticalArrangement={{ spacedBy: 20 }}
          horizontalAlignment="start"
        >
          <BackButton />
          <Slot />
        </Column>
      </Column>
    </Row>
  );
}
