import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column, Slider, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { Hardware, type HardwareState } from "../hardware/Hardware";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  ModeRow,
  SectionHeader,
  SettingsCard,
  StatusRow,
} from "../components/SettingsUI";
import { copy, numberFormat } from "../strings";

type RadioDevice = {
  interface: string;
  driver: string;
  perm_mac: string;
  mac?: string;
  address?: string;
  simulated?: boolean;
  selected?: boolean;
  bound?: boolean;
  name?: string;
  bus?: string;
};

export function HardwareSection() {
  const colors = useMaterialColors();
  const [hardware, setHardware] = useState<HardwareState | null>(null);
  const [audioDevices, setAudioDevices] = useState<Array<{ id: string; name: string; multiplier: number }>>([]);
  const [wifiDevices, setWifiDevices] = useState<RadioDevice[] | null>(null);
  const [bluetoothDevices, setBluetoothDevices] = useState<RadioDevice[] | null>(null);
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
    for (const [target, setDevices] of [
      ["wifi", setWifiDevices],
      ["bluetooth", setBluetoothDevices],
    ] as const) {
      try {
        const result = await MatonOS.call(target, "list_devices");
        if (!result.available) {
          console.error(`${target} device list unavailable: ${result.reason}`);
          setDevices(null);
          continue;
        }
        const state = JSON.parse(result.value) as { devices?: RadioDevice[] };
        setDevices(state.devices ?? []);
      } catch (error) {
        console.error(`${target} device list read failed`, error);
        setDevices(null);
      }
    }
  }, []);
  const selectRadioDevice = useCallback(async (target: "wifi" | "bluetooth", device: RadioDevice) => {
    const identity = device.perm_mac || device.address || "";
    if (!identity) {
      console.error(`${target} device has no selectable permanent address: ${device.interface}`);
      return;
    }
    try {
      const result = await MatonOS.call(target, "select_device", { device: identity, persist: true });
      if (!result.available) {
        console.error(`${target} selection unavailable for ${identity}: ${result.reason}`);
        return;
      }
      await refreshHardware();
    } catch (error) {
      console.error(`${target} selection failed for ${identity}`, error);
    }
  }, [refreshHardware]);
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
  const audioDeviceRows = audioDevices.map((device) => (
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
  ));
  if (audioDeviceRows.length === 0) {
    audioDeviceRows.push(
      <Text key="none" style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
        {copy.noAudioDevices}
      </Text>,
    );
  }
  const wifiDeviceRows = (wifiDevices ?? []).map((device) => {
    const active = device.interface === "wlan0";
    const identifier = device.perm_mac || device.mac || copy.unknown;
    const driver = device.driver || copy.unknown;
    const details = `${driver} · ${identifier}${device.simulated ? ` · ${copy.virtualRadio}` : ""}${active ? ` · ${copy.active}` : ""}`;
    return (
      <ModeRow
        key={`wifi-${device.interface}-${identifier}`}
        title={device.interface}
        description={details}
        selected={active}
        onSelect={() => void selectRadioDevice("wifi", device)}
      />
    );
  });
  const bluetoothDeviceRows = (bluetoothDevices ?? []).map((device) => {
    const identifier = device.perm_mac || device.address || copy.unknown;
    const active = device.selected === true;
    const driver = device.driver || copy.unknown;
    const details = `${device.name || device.interface} · ${driver} · ${identifier}${device.bus ? ` · ${device.bus}` : ""}${active ? ` · ${copy.active}` : ""}`;
    return (
      <ModeRow
        key={`bluetooth-${device.interface}-${identifier}`}
        title={device.interface}
        description={details}
        selected={active}
        onSelect={() => void selectRadioDevice("bluetooth", device)}
      />
    );
  });
  const powerStatus = hardware?.battery.pluggedIn
    ? copy.connectedToPower
    : copy.batteryOrDesktop;
  // The daemon lists are the source of truth; null means the list could not be read.
  const wifiStatus =
    wifiDevices === null
      ? copy.unknown
      : wifiDevices.some((device) => !device.simulated)
        ? copy.physicalAdapter
        : wifiDevices.length > 0
          ? copy.virtualRadio
          : copy.notDetected;
  const bluetoothStatus =
    bluetoothDevices === null
      ? copy.unknown
      : bluetoothDevices.length > 0
        ? copy.physicalController
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
          {wifiDeviceRows}
          <StatusRow label={copy.bluetooth} value={bluetoothStatus} />
          {bluetoothDeviceRows}
        </SettingsCard>
        <SettingsCard title={copy.audioGraphics}>
          <StatusRow label={copy.audioOutput} value={audioStatus} />
          {audioDeviceRows}
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
