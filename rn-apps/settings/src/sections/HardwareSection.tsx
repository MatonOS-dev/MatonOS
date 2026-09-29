import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { Hardware, type HardwareState } from "../hardware/Hardware";
import {
  ActionButton,
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { copy, numberFormat } from "../strings";

export function HardwareSection() {
  const [hardware, setHardware] = useState<HardwareState | null>(null);
  const refreshHardware = useCallback(async () => {
    try {
      setHardware(await Hardware.getState());
    } catch {
      setHardware(null);
    }
  }, []);
  useFocusEffect(
    useCallback(() => {
      void refreshHardware();
    }, [refreshHardware]),
  );
  const batteryStatus = !hardware
    ? copy.unknown
    : hardware.battery.present
      ? `${hardware.battery.level}%${hardware.battery.charging ? " · charging" : ""}`
      : copy.notDetected;
  const powerStatus = hardware?.battery.pluggedIn
    ? copy.connectedToPower
    : copy.batteryOrDesktop;
  const wifiStatus =
    !hardware || hardware.wifi.present === null
      ? copy.unknown
      : hardware.wifi.present
        ? copy.physicalAdapter
        : hardware.wifi.virtual
          ? copy.virtualRadio
          : copy.notDetected;
  const bluetoothStatus =
    !hardware || hardware.bluetooth.present === null
      ? copy.unknown
      : hardware.bluetooth.present
        ? copy.physicalController
        : hardware.bluetooth.virtual
          ? copy.virtualController
          : copy.notDetected;
  const audioStatus =
    !hardware || hardware.audio.present === null
      ? copy.unknown
      : hardware.audio.present
        ? copy.available
        : copy.notDetected;
  const graphicsStatus =
    !hardware || hardware.gpu.present === null
      ? copy.unknown
      : hardware.gpu.present
        ? hardware.gpu.software
          ? copy.software
          : copy.hardwareRenderer
        : copy.notDetected;
  const cameraCount = hardware
    ? numberFormat.format(hardware.camera.count)
    : copy.unknown;
  return (
    <>
      <SectionHeader title={copy.hardware} summary={copy.hardwareSummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.battery}>
          <StatusRow label={copy.status} value={batteryStatus} />
          <StatusRow label={copy.power} value={powerStatus} />
        </SettingsCard>
        <SettingsCard title={copy.wireless}>
          <StatusRow label={copy.wifi} value={wifiStatus} />
          <StatusRow label={copy.bluetooth} value={bluetoothStatus} />
        </SettingsCard>
        <SettingsCard title={copy.audioGraphics}>
          <StatusRow label={copy.audioOutput} value={audioStatus} />
          <StatusRow label={copy.graphicsRenderer} value={graphicsStatus} />
        </SettingsCard>
        <SettingsCard title={copy.camera}>
          <StatusRow label={copy.connectedCameras} value={cameraCount} />
        </SettingsCard>
        <ActionButton
          label={copy.refreshHardware}
          variant="text"
          onClick={() => void refreshHardware()}
        />
      </Column>
    </>
  );
}
