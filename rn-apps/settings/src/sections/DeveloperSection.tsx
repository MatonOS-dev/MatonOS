import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
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
import { GappsSection } from "./GappsSection";

type TrustedApp = {
  package: string;
  label: string;
  builtin: boolean;
  installed: boolean;
  targets: string[];
};
type TrustState = { allowRequests: boolean; apps: TrustedApp[] };

function parse<T>(value: string): T | null {
  try {
    return JSON.parse(value) as T;
  } catch {
    return null;
  }
}

function targetNames(targets: string[]) {
  const names = targets.map(
    (target) => copy.bridgeTargets[target as keyof typeof copy.bridgeTargets] ?? target,
  );
  return names.length ? names.join(", ") : copy.noCapabilities;
}

function AppCard({ app, onRevoke }: { app: TrustedApp; onRevoke: (app: TrustedApp) => void }) {
  const status = app.builtin ? copy.trustBuiltIn : copy.trustApproved;
  const revoke = app.builtin ? null : (
    <ActionButton label={copy.revokeTrust} variant="text" onClick={() => onRevoke(app)} />
  );
  const missing = app.installed ? null : <Text>{copy.trustNotInstalled}</Text>;
  return (
    <SettingsCard title={app.label}>
      <StatusRow label={app.package} value={status} supporting={targetNames(app.targets)} />
      {missing}
      {revoke}
    </SettingsCard>
  );
}

export function DeveloperSection() {
  const colors = useMaterialColors();
  const [state, setState] = useState<TrustState | null>(null);
  const [message, setMessage] = useState("");

  const refresh = useCallback(async () => {
    const result = await MatonOS.call("trust", "get_state");
    const parsed = result.available ? parse<TrustState>(result.value) : null;
    if (!parsed) {
      console.error("trust get_state failed", result.reason);
      setState(null);
      setMessage(result.reason || copy.trustUnavailable);
      return;
    }
    setState(parsed);
  }, []);
  useFocusEffect(
    useCallback(() => {
      void refresh();
    }, [refresh]),
  );

  const call = async (command: string, args: unknown) => {
    const result = await MatonOS.call("trust", command, args);
    if (!result.available) console.error(`trust ${command} failed`, result.reason);
    setMessage(result.available ? parse<string>(result.value) ?? copy.settingApplied : result.reason || copy.trustUnavailable);
    void refresh();
  };

  const cards = (state?.apps ?? []).map((app) => (
    <AppCard key={app.package} app={app} onRevoke={(target) => void call("revoke", { package: target.package })} />
  ));
  const note = message ? <Text color={colors.onSurfaceVariant}>{message}</Text> : null;

  return (
    <>
      <SectionHeader title={copy.developer} summary={copy.developerSummary} />
      <Column
        modifiers={[fillMaxWidth()]}
        verticalArrangement={{ spacedBy: 16 }}
        horizontalAlignment="start"
      >
        <SettingsCard title={copy.trustedApps}>
          <Text style={{ typography: "bodyMedium" }} color={colors.onSurfaceVariant}>
            {copy.trustedAppsDetails}
          </Text>
          <ToggleRow
            title={copy.allowTrustRequests}
            supporting={copy.allowTrustRequestsHint}
            value={state?.allowRequests ?? false}
            onChange={(allow) => void call("set_allow_requests", { allow })}
          />
        </SettingsCard>
        {cards}
        {note}
        <GappsSection />
      </Column>
    </>
  );
}
