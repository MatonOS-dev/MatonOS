import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import { Hardware } from "../hardware/Hardware";
import { SectionHeader, ModeRow, SettingsCard } from "../components/SettingsUI";

type Camera = { id: string; facing: string; jpeg: string; yuv: string };
type Roles = { front: string; back: string };

export function CameraRolesSection() {
  const colors = useMaterialColors();
  const [cameras, setCameras] = useState<Camera[]>([]);
  const [roles, setRoles] = useState<Roles>({ front: "", back: "" });
  const refresh = useCallback(async () => {
    try {
      setCameras(await Hardware.getCameras());
    } catch (error) {
      console.error("camera list read failed", error);
      setCameras([]);
    }
    try {
      const result = await MatonOS.call("camera", "get_roles");
      if (!result.available) throw new Error(`camera roles unavailable: ${result.reason}`);
      setRoles(JSON.parse(result.value) as Roles);
    } catch (error) {
      console.error("camera role read failed", error);
      setRoles({ front: "", back: "" });
    }
  }, []);
  const setRole = useCallback(async (role: "front" | "back", id: string) => {
    try {
      const result = await MatonOS.call("camera", "set_role", { role, id });
      if (!result.available) throw new Error(`camera role update unavailable: ${result.reason}`);
      setRoles(JSON.parse(result.value) as Roles);
    } catch (error) {
      console.error(`camera role update failed role=${role} id=${id}`, error);
    }
  }, []);
  useFocusEffect(useCallback(() => { void refresh(); }, [refresh]));
  const rows = cameras.map((camera) => {
    const facing = roles.front === camera.id ? "front" : roles.back === camera.id ? "back" : "external";
    const details = `JPEG ${camera.jpeg} · YUV ${camera.yuv}`;
    return (
      <Column key={`camera-${camera.id}`} modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 4 }}>
        <Text style={{ typography: "titleSmall" }} color={colors.onSurface}>{camera.id} · {details}</Text>
        <ModeRow title="Front" description="" selected={facing === "front"} onSelect={() => void setRole("front", camera.id)} />
        <ModeRow title="Back" description="" selected={facing === "back"} onSelect={() => void setRole("back", camera.id)} />
        <ModeRow title="External" description="" selected={facing === "external"} onSelect={() => {
          if (roles.front === camera.id) void setRole("front", "");
          if (roles.back === camera.id) void setRole("back", "");
        }} />
      </Column>
    );
  });
  if (rows.length === 0) {
    rows.push(<Text key="none" style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>No cameras found</Text>);
  }
  return (
    <>
      <SectionHeader title="Cameras" summary="Choose how each camera is reported" />
      <SettingsCard title="Camera roles">{rows}</SettingsCard>
    </>
  );
}
