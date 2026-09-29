import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import {
  Column,
  Row,
  Slider,
  Text,
  useMaterialColors,
} from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { MatonOS } from "../bridge/MatonOS";
import {
  ActionButton,
  ModeRow,
  SectionHeader,
  SettingsCard,
  StatusRow,
  ToggleRow,
} from "../components/SettingsUI";
import { copy, numberFormat } from "../strings";

type SleepMode = "automatic" | "suspend" | "hibernate";
type SleepState = {
  idleTimeoutSeconds: number;
  suspendSupported: boolean;
  sleeping: boolean;
};
function parse<T>(value: string): T | null {
  try {
    return JSON.parse(value) as T;
  } catch {
    return null;
  }
}

export function SleepSection() {
  const colors = useMaterialColors();
  const [sleepState, setSleepState] = useState<SleepState | null>(null);
  const [timeout, setTimeoutValue] = useState(0);
  const [sleepEnabled, setSleepEnabled] = useState(true);
  const [sleepMode, setSleepMode] = useState<SleepMode>("automatic");
  const [message, setMessage] = useState("");

  const refreshSleep = useCallback(async () => {
    setMessage("");
    const result = await MatonOS.call("sleep", "get_state");
    if (!result.available) {
      setSleepState(null);
      setMessage(result.reason || copy.timeoutServiceUnavailable);
      return;
    }
    const state = parse<SleepState>(result.value);
    if (!state) {
      setSleepState(null);
      setMessage(copy.unreadableSleepState);
      return;
    }
    setSleepState(state);
    setTimeoutValue(state.idleTimeoutSeconds);
    setSleepEnabled(state.idleTimeoutSeconds > 0);
  }, []);
  useFocusEffect(
    useCallback(() => {
      void refreshSleep();
    }, [refreshSleep]),
  );

  const callSleep = async (command: string, args: unknown) => {
    setMessage("");
    const result = await MatonOS.call("sleep", command, args);
    if (!result.available) {
      setMessage(result.reason || copy.sleepControlUnavailable);
      return false;
    }
    setMessage(result.value || copy.settingApplied);
    void refreshSleep();
    return true;
  };
  const setMode = async (mode: SleepMode) => {
    if (await callSleep("set_mode", { mode })) setSleepMode(mode);
  };
  const setIdleTimeout = async () => {
    const applied = await callSleep("set_idle_timeout", {
      seconds: sleepEnabled ? Math.round(timeout) : 0,
    });
    if (applied) setSleepEnabled(timeout > 0);
  };
  const stateLabel = sleepState?.sleeping ? copy.sleeping : copy.awake;
  const suspendLabel = sleepState
    ? sleepState.suspendSupported
      ? copy.available
      : copy.notDetected
    : copy.unknown;
  const timeoutLabel =
    !sleepEnabled || timeout === 0
      ? copy.never
      : `${numberFormat.format(Math.round(timeout / 60))} ${copy.minutes}`;

  return (
    <>
      <SectionHeader title={copy.sleep} summary={copy.powerIdle} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.sleepService}>
          <StatusRow
            label={copy.service}
            value={sleepState ? copy.connected : copy.unavailable}
            supporting={copy.ownsIdle}
          />
          <StatusRow label={copy.systemState} value={stateLabel} />
          <StatusRow label={copy.suspendSupport} value={suspendLabel} />
          <ActionButton
            label={copy.refreshStatus}
            variant="text"
            onClick={() => void refreshSleep()}
          />
        </SettingsCard>
        <SettingsCard title={copy.idleTimeout}>
          <ToggleRow
            title={copy.sleepWhenIdle}
            supporting={copy.keepAwake}
            value={sleepEnabled}
            onChange={setSleepEnabled}
          />
          <Text style={{ typography: "titleMedium" }} color={colors.onSurface}>
            {timeoutLabel}
          </Text>
          <Slider
            value={timeout}
            min={0}
            max={3600}
            steps={11}
            enabled={sleepEnabled}
            onValueChange={(value) => setTimeoutValue(Math.round(value))}
          />
          <Row
            modifiers={[fillMaxWidth()]}
            horizontalArrangement="spaceBetween"
            verticalAlignment="center"
          >
            <Text
              style={{ typography: "bodySmall" }}
              color={colors.onSurfaceVariant}
            >
              {copy.never}
            </Text>
            <Text
              style={{ typography: "bodySmall" }}
              color={colors.onSurfaceVariant}
            >
              {numberFormat.format(60)} min
            </Text>
          </Row>
          <ActionButton
            label={copy.applyTimeout}
            enabled={!!sleepState}
            onClick={() => void setIdleTimeout()}
          />
        </SettingsCard>
        <SettingsCard title={copy.sleepMode}>
          <ModeRow
            title={copy.automatic}
            description={copy.automaticDescription}
            selected={sleepMode === "automatic"}
            onSelect={() => void setMode("automatic")}
          />
          <ModeRow
            title={copy.suspend}
            description={copy.suspendDescription}
            selected={sleepMode === "suspend"}
            onSelect={() => void setMode("suspend")}
          />
          <ModeRow
            title={copy.hibernate}
            description={copy.hibernateDescription}
            selected={sleepMode === "hibernate"}
            onSelect={() => void setMode("hibernate")}
          />
        </SettingsCard>
        {message ? (
          <Text
            style={{ typography: "bodyMedium" }}
            color={colors.onSurfaceVariant}
          >
            {message}
          </Text>
        ) : null}
      </Column>
    </>
  );
}
