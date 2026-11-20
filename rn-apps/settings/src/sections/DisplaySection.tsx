import { useCallback, useEffect, useState } from "react";
import { useFocusEffect } from "expo-router";
import {
  Button,
  Column,
  DropdownMenu,
  DropdownMenuItem,
  FlowRow,
  Text,
  useMaterialColors,
} from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  SectionHeader,
  SettingsCard,
  StatusRow,
  ToggleRow,
} from "../components/SettingsUI";
import { copy } from "../strings";

type DisplayMode = { width: number; height: number; refresh: number };
type Monitor = DisplayMode & {
  id: number;
  name: string;
  external: boolean;
  enabled: boolean;
  primary: boolean;
  modes: DisplayMode[];
  rotation: number;
  scale: number;
  bounds?: { left: number; top: number; width: number; height: number };
};
type DisplayState = { mirror: boolean; displays: Monitor[] };
type Call = (command: string, args: unknown) => Promise<void>;

const SCALES = [100, 125, 150, 175, 200];
const ROTATIONS = [0, 1, 2, 3];
const SIDES = ["left", "right", "above", "below"] as const;

function parse<T>(value: string): T | null {
  try {
    return JSON.parse(value) as T;
  } catch {
    return null;
  }
}

function modeLabel(mode: DisplayMode) {
  return `${mode.width} × ${mode.height} · ${Math.round(mode.refresh)} Hz`;
}

function sideOf(monitor: Monitor, primary: Monitor | undefined) {
  if (!monitor.bounds || !primary?.bounds) return "";
  const a = primary.bounds;
  const b = monitor.bounds;
  if (b.left >= a.left + a.width) return "right";
  if (b.left + b.width <= a.left) return "left";
  if (b.top + b.height <= a.top) return "above";
  return "below";
}

function ChoiceButton({
  label,
  selected,
  onClick,
}: {
  label: string;
  selected: boolean;
  onClick: () => void;
}) {
  return (
    <ActionButton
      label={label}
      variant={selected ? "filled" : "text"}
      onClick={onClick}
    />
  );
}

function ResolutionPicker({ monitor, call }: { monitor: Monitor; call: Call }) {
  const [open, setOpen] = useState(false);
  const pick = (mode: DisplayMode) => {
    setOpen(false);
    void call("set_mode", { id: monitor.id, ...mode });
  };
  const items = monitor.modes.map((mode) => (
    <DropdownMenuItem
      key={modeLabel(mode)}
      onClick={() => pick(mode)}
    >
      <DropdownMenuItem.Text>
        <Text>{modeLabel(mode)}</Text>
      </DropdownMenuItem.Text>
    </DropdownMenuItem>
  ));
  return (
    <DropdownMenu expanded={open} onDismissRequest={() => setOpen(false)}>
      <DropdownMenu.Trigger>
        <Button onClick={() => setOpen(true)}>
          <Text>{modeLabel(monitor)}</Text>
        </Button>
      </DropdownMenu.Trigger>
      <DropdownMenu.Items>{items}</DropdownMenu.Items>
    </DropdownMenu>
  );
}

function ExternalControls({
  monitor,
  primary,
  call,
}: {
  monitor: Monitor;
  primary: Monitor | undefined;
  call: Call;
}) {
  const current = sideOf(monitor, primary);
  const sides = SIDES.map((side) => (
    <ChoiceButton
      key={side}
      label={copy.displaySides[side]}
      selected={current === side}
      onClick={() => void call("set_position", { id: monitor.id, side })}
    />
  ));
  const arrangement = monitor.enabled && !monitor.primary ? (
    <Column verticalArrangement={{ spacedBy: 4 }}>
      <Text style={{ typography: "titleSmall" }}>{copy.displayPosition}</Text>
      <FlowRow horizontalArrangement={{ spacedBy: 8 }}>{sides}</FlowRow>
    </Column>
  ) : null;
  return (
    <>
      <ToggleRow
        title={copy.displayUse}
        supporting={copy.displayUseHint}
        value={monitor.enabled}
        onChange={(enabled) => void call("set_enabled", { id: monitor.id, enabled })}
      />
      {arrangement}
    </>
  );
}

function MonitorCard({
  monitor,
  primary,
  call,
}: {
  monitor: Monitor;
  primary: Monitor | undefined;
  call: Call;
}) {
  const kind = monitor.external ? copy.displayExternal : copy.displayBuiltIn;
  const status = monitor.enabled ? modeLabel(monitor) : copy.displayOff;
  const scales = SCALES.map((percent) => (
    <ChoiceButton
      key={percent}
      label={`${percent}%`}
      selected={monitor.scale === percent}
      onClick={() => void call("set_scale", { id: monitor.id, percent })}
    />
  ));
  const rotations = ROTATIONS.map((rotation) => (
    <ChoiceButton
      key={rotation}
      label={`${rotation * 90}°`}
      selected={monitor.rotation === rotation}
      onClick={() => void call("set_rotation", { id: monitor.id, rotation })}
    />
  ));
  const external = monitor.external ? (
    <ExternalControls monitor={monitor} primary={primary} call={call} />
  ) : null;
  const settings = monitor.enabled ? (
    <Column verticalArrangement={{ spacedBy: 8 }}>
      <Text style={{ typography: "titleSmall" }}>{copy.displayResolution}</Text>
      <ResolutionPicker monitor={monitor} call={call} />
      <Text style={{ typography: "titleSmall" }}>{copy.displayScale}</Text>
      <FlowRow horizontalArrangement={{ spacedBy: 8 }}>{scales}</FlowRow>
      <Text style={{ typography: "titleSmall" }}>{copy.displayRotation}</Text>
      <FlowRow horizontalArrangement={{ spacedBy: 8 }}>{rotations}</FlowRow>
    </Column>
  ) : null;
  return (
    <SettingsCard title={monitor.name}>
      <StatusRow label={kind} value={status} />
      {external}
      {settings}
    </SettingsCard>
  );
}

export function DisplaySection() {
  const colors = useMaterialColors();
  const [state, setState] = useState<DisplayState | null>(null);
  const [message, setMessage] = useState("");

  const refresh = useCallback(async () => {
    const result = await MatonOS.call("display", "get_state");
    if (!result.available) {
      setState(null);
      setMessage(result.reason || copy.displayUnavailable);
      return;
    }
    const parsed = parse<DisplayState>(result.value);
    if (!parsed) {
      setState(null);
      setMessage(copy.displayUnreadable);
      return;
    }
    setState(parsed);
  }, []);
  useFocusEffect(
    useCallback(() => {
      void refresh();
    }, [refresh]),
  );
  // Monitors plugged in or out while the page is open.
  useEffect(() => {
    const subscription = MatonOS.subscribe("display", "changed", () => void refresh());
    return () => subscription.remove();
  }, [refresh]);

  const call: Call = async (command, args) => {
    const result = await MatonOS.call("display", command, args);
    if (!result.available) {
      console.error(`display ${command} failed`, result.reason);
      setMessage(result.reason || copy.displayUnavailable);
    } else {
      setMessage(parse<string>(result.value) ?? copy.settingApplied);
    }
    void refresh();
  };

  const monitors = state?.displays ?? [];
  const primary = monitors.find((monitor) => monitor.primary);
  const hasExternal = monitors.some((monitor) => monitor.external);
  const cards = monitors.map((monitor) => (
    <MonitorCard key={monitor.id} monitor={monitor} primary={primary} call={call} />
  ));
  const mirror = hasExternal ? (
    <SettingsCard title={copy.displayMultiple}>
      <ToggleRow
        title={copy.displayMirror}
        supporting={copy.displayMirrorHint}
        value={state?.mirror ?? false}
        onChange={(value) => void call("set_mirror", { mirror: value })}
      />
    </SettingsCard>
  ) : null;
  const note = message ? <Text color={colors.onSurfaceVariant}>{message}</Text> : null;

  return (
    <>
      <SectionHeader title={copy.display} summary={copy.displaySummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        {mirror}
        {cards}
        {note}
      </Column>
    </>
  );
}
