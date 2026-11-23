import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column, Slider, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { Hardware, type HardwareState } from "../hardware/Hardware";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { copy, numberFormat } from "../strings";

export function HardwareSection() {
  const colors = useMaterialColors();
  const [hardware, setHardware] = useState<HardwareState | null>(null);
  const [audioDevices, setAudioDevices] = useState<Array<{ id: string; name: string; multiplier: number }>>([]);
  const refreshHardware = useCallback(async () => {
    try {
      setHardware(await Hardware.getState());
    } catch {
      setHardware(null);
    }
    try {
      const result = await MatonOS.call("audio", "get_devices");
      if (result.available) {
        const state = JSON.parse(result.value) as { devices?: typeof audioDevices };
        setAudioDevices(state.devices ?? []);
      } else {
        setAudioDevices([]);
      }
    } catch (error) {
      console.error("audio device list read failed", error);
      setAudioDevices([]);
    }
  }, []);
  const updateDeviceBoost = useCallback((id: string, value: number) => {
    const multiplier = Math.max(0.5, Math.min(1.5, Math.round(value * 100) / 100));
    setAudioDevices((devices) => devices.map((device) => device.id === id ? { ...device, multiplier } : device));
    void MatonOS.call("audio", "set_boost", { id, multiplier }).then((result) => {
      if (!result.available) console.error(`audio boost update unavailable: ${result.reason}`);
    }).catch((error) => console.error(`audio boost update failed id=${id}`, error));
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
          {audioDevices.length === 0 ? (
            <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
              {copy.noAudioDevices}
            </Text>
          ) : audioDevices.map((device) => (
            <Column key={device.id} modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 6 }}>
              <Text style={{ typography: "titleMedium" }} color={colors.onSurface}>
                {device.name}: {device.multiplier.toFixed(2)}×
              </Text>
              <Slider
                value={Math.round(device.multiplier * 100)}
                min={50}
                max={150}
                steps={99}
                onValueChange={(value) => updateDeviceBoost(device.id, value / 100)}
              />
            </Column>
          ))}
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
