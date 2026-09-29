import { useEffect, useState, type ReactNode } from "react";
import {
  unstable_getMaterialSymbolSourceAsync,
  type AndroidSymbol,
} from "expo-symbols";
import { type ImageSourcePropType } from "react-native";
import {
  Card,
  Column,
  FilledTonalButton,
  Icon,
  ListItem,
  RadioButton,
  Row,
  Switch,
  Text,
  TextButton,
  useMaterialColors,
} from "@expo/ui/jetpack-compose";
import {
  fillMaxHeight,
  fillMaxWidth,
  paddingAll,
  weight,
  width,
} from "@expo/ui/jetpack-compose/modifiers";
import { usePathname, useRouter } from "expo-router";
import { MatonOS } from "../bridge/MatonOS";
import { copy } from "../strings";
import bedtimeIcon from "../../assets/icons/bedtime.xml";
import devicesIcon from "../../assets/icons/devices.xml";
import developerIcon from "../../assets/icons/developer.xml";
import infoIcon from "../../assets/icons/info.xml";
import installIcon from "../../assets/icons/install.xml";

type Section = "install" | "sleep" | "hardware" | "developer" | "about";
type NavEntry = {
  id: Section;
  title: string;
  symbol: AndroidSymbol;
  fallback: number;
};
const navItems: Record<Section, NavEntry> = {
  install: {
    id: "install",
    title: copy.install,
    symbol: "download",
    fallback: installIcon,
  },
  sleep: {
    id: "sleep",
    title: copy.sleep,
    symbol: "bedtime",
    fallback: bedtimeIcon,
  },
  hardware: {
    id: "hardware",
    title: copy.hardware,
    symbol: "devices",
    fallback: devicesIcon,
  },
  developer: {
    id: "developer",
    title: copy.developer,
    symbol: "code",
    fallback: developerIcon,
  },
  about: { id: "about", title: copy.about, symbol: "info", fallback: infoIcon },
};
const symbolRequests = new Map<string, Promise<ImageSourcePropType | null>>();
const symbolSources = new Map<string, ImageSourcePropType>();
function resolveSymbol(symbol: AndroidSymbol) {
  let request = symbolRequests.get(symbol);
  if (!request) {
    request = unstable_getMaterialSymbolSourceAsync(symbol, 24, "#000000");
    symbolRequests.set(symbol, request);
    void request
      .then((source) => {
        if (source) symbolSources.set(symbol, source);
      })
      .catch(() => {});
  }
  return request;
}
export function useSymbolSource(
  symbol: AndroidSymbol,
  fallback: number,
): ImageSourcePropType {
  const [resolved, setResolved] = useState<{
    symbol: string;
    source: ImageSourcePropType;
  } | null>(null);
  useEffect(() => {
    let active = true;
    if (symbolSources.has(symbol)) return;
    void resolveSymbol(symbol)
      .then((source) => {
        if (active && source) setResolved({ symbol, source });
      })
      .catch(() => {});
    return () => {
      active = false;
    };
  }, [symbol]);
  return (
    symbolSources.get(symbol) ??
    (resolved?.symbol === symbol ? resolved.source : fallback)
  );
}

export function SettingsCard({
  title,
  children,
}: {
  title: string;
  children: ReactNode;
}) {
  const colors = useMaterialColors();
  return (
    <Card
      colors={{
        containerColor: colors.surfaceContainerLow,
        contentColor: colors.onSurface,
      }}
      modifiers={[fillMaxWidth()]}
    >
      <Column
        modifiers={[fillMaxWidth(), paddingAll(20)]}
        verticalArrangement={{ spacedBy: 12 }}
        horizontalAlignment="start"
      >
        <Text style={{ typography: "titleLarge" }} color={colors.onSurface}>
          {title}
        </Text>
        {children}
      </Column>
    </Card>
  );
}

const rowSymbols: Record<string, AndroidSymbol> = {
  [copy.service]: "power_settings_new",
  [copy.systemState]: "monitor_heart",
  [copy.suspendSupport]: "bolt",
  [copy.minimumDrive]: "storage",
  [copy.slotPlan]: "splitscreen",
  [copy.capacity]: "database",
  [copy.connection]: "cable",
  [copy.device]: "devices",
  [copy.status]: "battery_full",
  [copy.power]: "power",
  [copy.wifi]: "wifi",
  [copy.bluetooth]: "bluetooth",
  [copy.audioOutput]: "volume_up",
  [copy.graphicsRenderer]: "desktop_windows",
  [copy.connectedCameras]: "photo_camera",
  [copy.osVersion]: "info",
  [copy.build]: "terminal",
  [copy.sdkLevel]: "developer_board",
};
export function StatusRow({
  label,
  value,
  supporting,
}: {
  label: string;
  value: string;
  supporting?: string;
}) {
  const colors = useMaterialColors();
  const iconSource = useSymbolSource(rowSymbols[label] ?? "info", infoIcon);
  return (
    <ListItem modifiers={[fillMaxWidth()]}>
      <ListItem.HeadlineContent>
        <Row verticalAlignment="center" horizontalArrangement="start">
          <Icon
            source={iconSource}
            size={20}
            tint={colors.onSurfaceVariant}
            contentDescription={label}
          />
          <Text style={{ typography: "bodyLarge" }} color={colors.onSurface}>
            {label}
          </Text>
        </Row>
      </ListItem.HeadlineContent>
      {supporting ? (
        <ListItem.SupportingContent>
          <Text
            style={{ typography: "bodySmall" }}
            color={colors.onSurfaceVariant}
          >
            {supporting}
          </Text>
        </ListItem.SupportingContent>
      ) : null}
      <ListItem.TrailingContent>
        <Text
          style={{ typography: "bodyMedium" }}
          color={colors.onSurfaceVariant}
        >
          {value}
        </Text>
      </ListItem.TrailingContent>
    </ListItem>
  );
}

export function ActionButton({
  label,
  onClick,
  enabled = true,
  variant = "filled",
}: {
  label: string;
  onClick: () => void;
  enabled?: boolean;
  variant?: "filled" | "text";
}) {
  const colors = useMaterialColors();
  const content = (
    <Row
      verticalAlignment="center"
      horizontalArrangement="center"
      modifiers={[paddingAll(2)]}
    >
      <Text
        style={{ typography: "labelLarge" }}
        color={
          variant === "filled" ? colors.onPrimaryContainer : colors.primary
        }
      >
        {label}
      </Text>
    </Row>
  );
  return variant === "filled" ? (
    <FilledTonalButton
      onClick={onClick}
      enabled={enabled}
      colors={{
        containerColor: colors.primaryContainer,
        contentColor: colors.onPrimaryContainer,
        disabledContainerColor: colors.surfaceVariant,
        disabledContentColor: colors.onSurfaceVariant,
      }}
      modifiers={[paddingAll(4)]}
    >
      {content}
    </FilledTonalButton>
  ) : (
    <TextButton
      onClick={onClick}
      enabled={enabled}
      colors={{
        contentColor: colors.primary,
        disabledContentColor: colors.onSurfaceVariant,
      }}
    >
      {content}
    </TextButton>
  );
}

export function ModeRow({
  title,
  description,
  selected,
  onSelect,
}: {
  title: string;
  description: string;
  selected: boolean;
  onSelect: () => void;
}) {
  const colors = useMaterialColors();
  return (
    <Row
      modifiers={[fillMaxWidth(), paddingAll(4)]}
      verticalAlignment="center"
      horizontalArrangement="start"
    >
      <RadioButton
        selected={selected}
        onClick={onSelect}
        colors={{
          selectedColor: colors.primary,
          unselectedColor: colors.outline,
        }}
      />
      <Column
        modifiers={[weight(1), paddingAll(8)]}
        verticalArrangement={{ spacedBy: 3 }}
        horizontalAlignment="start"
      >
        <Text style={{ typography: "bodyLarge" }} color={colors.onSurface}>
          {title}
        </Text>
        <Text
          style={{ typography: "bodySmall" }}
          color={colors.onSurfaceVariant}
        >
          {description}
        </Text>
      </Column>
    </Row>
  );
}

export function ToggleRow({
  title,
  supporting,
  value,
  onChange,
}: {
  title: string;
  supporting: string;
  value: boolean;
  onChange: (value: boolean) => void;
}) {
  const colors = useMaterialColors();
  return (
    <ListItem modifiers={[fillMaxWidth()]}>
      <ListItem.HeadlineContent>
        <Text style={{ typography: "bodyLarge" }} color={colors.onSurface}>
          {title}
        </Text>
      </ListItem.HeadlineContent>
      <ListItem.SupportingContent>
        <Text
          style={{ typography: "bodySmall" }}
          color={colors.onSurfaceVariant}
        >
          {supporting}
        </Text>
      </ListItem.SupportingContent>
      <ListItem.TrailingContent>
        <Switch value={value} onCheckedChange={onChange} />
      </ListItem.TrailingContent>
    </ListItem>
  );
}

export function SectionHeader({
  title,
  summary,
}: {
  title: string;
  summary: string;
}) {
  const colors = useMaterialColors();
  return (
    <Column
      modifiers={[fillMaxWidth(), paddingAll(4)]}
      verticalArrangement={{ spacedBy: 4 }}
      horizontalAlignment="start"
    >
      <Text style={{ typography: "headlineLarge" }} color={colors.onSurface}>
        {title}
      </Text>
      <Text style={{ typography: "bodyLarge" }} color={colors.onSurfaceVariant}>
        {summary}
      </Text>
    </Column>
  );
}

function NavItem({
  section,
  selected,
}: {
  section: Section;
  selected: boolean;
}) {
  const colors = useMaterialColors();
  const router = useRouter();
  const item = navItems[section];
  const icon = useSymbolSource(item.symbol, item.fallback);
  const onClick = () => router.push(section === "sleep" ? "/" : `/${section}`);
  const content = (
    <Row
      modifiers={[fillMaxWidth(), paddingAll(4)]}
      verticalAlignment="center"
      horizontalArrangement="start"
    >
      <Icon
        source={icon}
        size={22}
        tint={selected ? colors.onSecondaryContainer : colors.onSurfaceVariant}
        contentDescription={item.title}
      />
      <Text
        style={{ typography: "labelLarge" }}
        color={selected ? colors.onSecondaryContainer : colors.onSurfaceVariant}
      >
        {item.title}
      </Text>
    </Row>
  );
  return selected ? (
    <FilledTonalButton
      onClick={onClick}
      colors={{
        containerColor: colors.secondaryContainer,
        contentColor: colors.onSecondaryContainer,
      }}
      modifiers={[fillMaxWidth()]}
    >
      {content}
    </FilledTonalButton>
  ) : (
    <TextButton
      onClick={onClick}
      colors={{ contentColor: colors.onSurfaceVariant }}
      modifiers={[fillMaxWidth()]}
    >
      {content}
    </TextButton>
  );
}

function LiveOnly({ live, children }: { live: boolean; children: ReactNode }) {
  return live ? <>{children}</> : null;
}

export function NavRail({ live }: { live: boolean }) {
  const colors = useMaterialColors();
  const pathname = usePathname();
  const current = pathname === "/" ? "sleep" : pathname.slice(1);
  return (
    <Column
      modifiers={[width(248), fillMaxHeight(), paddingAll(16)]}
      verticalArrangement={{ spacedBy: 8 }}
      horizontalAlignment="start"
    >
      <Text style={{ typography: "headlineSmall" }} color={colors.onSurface}>
        {copy.app}
      </Text>
      <Text style={{ typography: "bodySmall" }} color={colors.onSurfaceVariant}>
        {copy.settings}
      </Text>
      <Column
        modifiers={[fillMaxWidth(), paddingAll(4)]}
        verticalArrangement={{ spacedBy: 8 }}
      >
        <LiveOnly live={live}>
          <NavItem section="install" selected={current === "install"} />
        </LiveOnly>
        <NavItem section="sleep" selected={current === "sleep"} />
        <NavItem section="hardware" selected={current === "hardware"} />
        <NavItem section="developer" selected={current === "developer"} />
        <NavItem section="about" selected={current === "about"} />
      </Column>
    </Column>
  );
}

export function BackButton() {
  const colors = useMaterialColors();
  const router = useRouter();
  const icon = useSymbolSource("arrow_back", infoIcon);
  return (
    <TextButton
      onClick={() => {
        if (router.canGoBack()) router.back();
        else void MatonOS.finishActivity();
      }}
      colors={{ contentColor: colors.primary }}
    >
      <Row
        verticalAlignment="center"
        horizontalArrangement="center"
        modifiers={[paddingAll(4)]}
      >
        <Icon
          source={icon}
          size={20}
          tint={colors.primary}
          contentDescription={copy.back}
        />
        <Text style={{ typography: "labelLarge" }} color={colors.primary}>
          {copy.back}
        </Text>
      </Row>
    </TextButton>
  );
}
