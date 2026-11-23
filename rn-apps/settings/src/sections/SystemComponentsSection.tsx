import { useCallback, useState } from "react";
import { Alert } from "react-native";
import { useFocusEffect } from "expo-router";
import { Column, Text, useMaterialColors } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { copy } from "../strings";
import { ModeRow, SectionHeader, SettingsCard } from "../components/SettingsUI";
import {
  SystemComponents,
  type ProviderChoices,
  type SystemComponentCandidate,
} from "../system/SystemComponents";

type Kind = "captions" | "tray";
type State = { captions: ProviderChoices; tray: ProviderChoices };

const defaults: Record<Kind, { package: string; label: string }> = {
  captions: { package: "org.matonos.caption", label: copy.defaultCaptionProvider },
  tray: { package: "org.matonos.tray", label: copy.defaultTrayHost },
};

function ProviderChoice({
  candidate,
  selected,
  onSelect,
}: {
  candidate: SystemComponentCandidate;
  selected: boolean;
  onSelect: () => void;
}) {
  return (
    <ModeRow
      title={candidate.label}
      description={`${candidate.package} · ${candidate.component}`}
      selected={selected}
      onSelect={onSelect}
    />
  );
}

function ProviderChoicesCard({
  kind,
  title,
  choices,
  onSelect,
}: {
  kind: Kind;
  title: string;
  choices: ProviderChoices;
  onSelect: (kind: Kind, component: string) => void;
}) {
  const defaultChoice = defaults[kind];
  const defaultSelected = !choices.current || !choices.candidates.some(
    (candidate) => candidate.component === choices.current,
  );
  const options = choices.candidates.map((candidate) => (
    <ProviderChoice
      key={candidate.component}
      candidate={candidate}
      selected={choices.current === candidate.component}
      onSelect={() => onSelect(kind, candidate.component)}
    />
  ));
  return (
    <SettingsCard title={title}>
      <ModeRow
        title={defaultChoice.label}
        description={copy.systemDefaultDescription}
        selected={defaultSelected}
        onSelect={() => onSelect(kind, "")}
      />
      {options}
    </SettingsCard>
  );
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export function SystemComponentsSection() {
  const colors = useMaterialColors();
  const [state, setState] = useState<State | null>(null);
  const [message, setMessage] = useState("");

  const refresh = useCallback(async () => {
    try {
      setState(await SystemComponents.read());
      setMessage("");
    } catch (error) {
      console.error("system component discovery failed", error);
      setMessage(`${copy.systemComponentsUnavailable}: ${errorText(error)}`);
    }
  }, []);
  useFocusEffect(useCallback(() => { void refresh(); }, [refresh]));

  const saveSelection = async (kind: Kind, component: string) => {
    try {
      await SystemComponents.select(kind, component);
      await refresh();
    } catch (error) {
      console.error(`system component selection failed kind=${kind} component=${component}`, error);
      setMessage(`${copy.systemComponentSaveFailed}: ${errorText(error)}`);
    }
  };

  const select = (kind: Kind, component: string) => {
    const candidate = state?.[kind].candidates.find((item) => item.component === component);
    if (!candidate || candidate.package === defaults[kind].package) {
      void saveSelection(kind, component);
      return;
    }
    Alert.alert(
      copy.privilegedUiConfirmationTitle,
      copy.privilegedUiConfirmation.replace("{app}", candidate.label),
      [
        { text: copy.cancel, style: "cancel" },
        { text: copy.continue, onPress: () => { void saveSelection(kind, component); } },
      ],
    );
  };

  const status = message ? <Text color={colors.onSurfaceVariant}>{message}</Text> : null;
  const captionCard = state ? (
    <ProviderChoicesCard kind="captions" title={copy.windowCaption} choices={state.captions} onSelect={select} />
  ) : null;
  const trayCard = state ? (
    <ProviderChoicesCard kind="tray" title={copy.trayHost} choices={state.tray} onSelect={select} />
  ) : null;
  return (
    <>
      <SectionHeader title={copy.systemComponents} summary={copy.systemComponentsSummary} />
      <Column modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 16 }} horizontalAlignment="start">
        {captionCard}
        {trayCard}
        {status}
      </Column>
    </>
  );
}
