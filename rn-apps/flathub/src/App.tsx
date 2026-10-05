import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { FlatList, View } from "react-native";
import { Image as ComposeImage, Button, Column, Host, LazyColumn, LinearProgressIndicator, OutlinedTextField, Row, Text, RNHostView } from "@expo/ui/jetpack-compose";
import { useMaterialColors } from "@expo/ui/jetpack-compose";
import { clickable, height, padding, paddingAll, size, weight, width } from "@expo/ui/jetpack-compose/modifiers";
import { getAppById, getAppDetails, getCollection, searchApps, type StoreApp } from "./FlathubApi";
import { fetchIconBase64, getInstalledRefs, installApp, runApp, uninstallApp, type ProgressEvent } from "./FlatpakBridge";
import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";

type Page = "browse" | "installed";
type Feed = "popular" | "new";

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : "Something went wrong.";
}

function Show({ when, children }: { when: boolean; children: ReactNode }) {
  if (!when) return null;
  return <>{children}</>;
}

function AppIcon({ app, imageSize }: { app: StoreApp; imageSize: number }) {
  if (!app.icon) return null;
  return <ComposeImage source={{ uri: app.icon }} contentDescription={`${app.name} icon`} modifiers={[size(imageSize, imageSize)]} />;
}

function ProgressLine({ progress }: { progress: string }) {
  if (!progress) return null;
  return <Text style={{ typography: "bodyMedium" }} maxLines={2}>{progress}</Text>;
}

function busyButtonLabel(kind: "install" | "uninstall"): string {
  return kind === "install" ? "Installing…" : "Working…";
}

function percentToRatio(percent: number | null): number | null {
  if (percent == null) return null;
  return Math.min(1, Math.max(0, percent / 100));
}

function Screenshots({ app }: { app: StoreApp }) {
  if (app.screenshots.length === 0) return null;
  return (
    <>
      <Text style={{ typography: "titleMedium" }}>Screenshots</Text>
      <RNHostView modifiers={[height(349)]}><FlatList removeClippedSubviews={false} horizontal style={{ height: 349 }} data={app.screenshots} keyExtractor={(_, index) => `${app.id}-${index}`}
        renderItem={({ item: src }) => <Host style={{ width: 620, height: 349 }} colorScheme="dark" seedColor="#79c75b"><ComposeImage source={{ uri: src }} contentDescription={`${app.name} screenshot`} modifiers={[width(620), height(349)]} /></Host>} /></RNHostView>
    </>
  );
}

function AppTile({ app, installed, busy, busyLabel, progress, percent, onOpenDetails, onInstall, onLaunch }: {
  app: StoreApp;
  installed: boolean;
  busy: boolean;
  busyLabel: string;
  progress: string;
  percent: number | null;
  onOpenDetails: (app: StoreApp) => void;
  onInstall: (app: StoreApp) => void;
  onLaunch: (app: StoreApp) => void;
}) {
  const isBusy = busy && Boolean(progress);
  const buttonLabel = isBusy ? (busyLabel || "Working…") : installed ? "Open" : "Install";
  const buttonAction = installed ? () => onLaunch(app) : () => onInstall(app);
  return (
    <Column modifiers={[clickable(() => onOpenDetails(app))]}>
      <Row verticalAlignment="center" modifiers={[paddingAll(12)]}>
        <AppIcon app={app} imageSize={52} />
        <Column modifiers={[weight(1), padding(12, 0, 12, 0)]}>
          <Text style={{ typography: "titleMedium" }}>{app.name}</Text>
          <Text style={{ typography: "bodyMedium" }} maxLines={2}>{app.summary}</Text>
        </Column>
        <Button enabled={!busy} onClick={buttonAction}><Text>{buttonLabel}</Text></Button>
      </Row>
      <Show when={isBusy}>
        <LinearProgressIndicator progress={percentToRatio(percent)} modifiers={[padding(0, 12, 12, 12)]} />
      </Show>
    </Column>
  );
}

function DetailContent({ app, installed, busy, busyLabel, progress, percent, onBack, onInstall, onUninstall }: {
  app: StoreApp; installed: boolean; busy: boolean; busyLabel: string; progress: string; percent: number | null; onBack: () => void; onInstall: () => void; onUninstall: () => void;
}) {
  const permissionText = app.permissions.length ? app.permissions.join(" · ") : "Permission details are not included for this app by the Flathub API.";
  const actionLabel = busy ? (busyLabel || "Working…") : installed ? "Uninstall" : "Install";
  const action = installed ? onUninstall : onInstall;
  return (
    <LazyColumn contentPadding={{ start: 20, top: 20, end: 20, bottom: 20 }} verticalArrangement={{ spacedBy: 14 }}>
      <Button onClick={onBack}><Text>‹  Back to apps</Text></Button>
      <Row verticalAlignment="center">
        <AppIcon app={app} imageSize={84} />
        <Column modifiers={[padding(18, 0, 18, 0)]}>
          <Text style={{ typography: "headlineSmall" }}>{app.name}</Text>
          <Text style={{ typography: "bodyLarge" }}>{app.summary}</Text>
          <Text style={{ typography: "bodySmall" }}>{app.developer}</Text>
        </Column>
      </Row>
      <Row horizontalArrangement={{ spacedBy: 10 }} verticalAlignment="center">
        <Button enabled={!busy} onClick={action}><Text>{actionLabel}</Text></Button>
        <Column modifiers={[weight(1)]}>
          <Show when={Boolean(progress)}><LinearProgressIndicator progress={percentToRatio(percent)} /></Show>
          <ProgressLine progress={progress} />
        </Column>
      </Row>
      <Screenshots app={app} />
      <Text style={{ typography: "titleMedium" }}>Permissions</Text>
      <Text style={{ typography: "bodyMedium" }}>{permissionText}</Text>
      <Text style={{ typography: "titleMedium" }}>About this app</Text>
      <Text style={{ typography: "bodyMedium" }}>{app.description.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim() || app.summary}</Text>
      <Text style={{ typography: "bodySmall" }}>{app.id}</Text>
    </LazyColumn>
  );
}

export default function App() {
  const themeColors = useMaterialColors({ colorScheme: "dark", seedColor: "#79c75b" });
  const [page, setPage] = useState<Page>("browse");
  const [feed, setFeed] = useState<Feed>("popular");
  const [apps, setApps] = useState<StoreApp[]>([]);
  const [installedRefs, setInstalledRefs] = useState<string[]>([]);
  const [installedApps, setInstalledApps] = useState<StoreApp[]>([]);
  const [installedError, setInstalledError] = useState("");
  const [selected, setSelected] = useState<StoreApp | null>(null);
  const [loading, setLoading] = useState(false);
  const [message, setMessage] = useState("");
  const [busyRef, setBusyRef] = useState("");
  const [progress, setProgress] = useState("");
  const [percent, setPercent] = useState<number | null>(null);
  const operationToken = useRef(0);
  const operationIdRef = useRef("");
  const operationKindRef = useRef<"install" | "uninstall">("install");
  const installedIds = useMemo(() => new Set(installedRefs.map((ref) => ref.split("/")[1])), [installedRefs]);

  async function refreshInstalled() {
    setInstalledError("");
    try {
      const refs = await getInstalledRefs();
      setInstalledRefs(refs);
      const entries = await Promise.all(refs.map(async (ref) => {
        const id = ref.split("/")[1];
        try { return await getAppById(id); }
        catch { return { id, name: id, summary: "Installed Flatpak", icon: "", developer: "", ref, description: "", screenshots: [], permissions: [] }; }
      }));
      setInstalledApps(entries);
    } catch (error) {
      setInstalledRefs([]); setInstalledApps([]); setInstalledError(errorText(error));
    }
  }

  async function loadCollection(nextFeed: Feed) {
    setLoading(true); setMessage("");
    try { setApps(await getCollection(nextFeed === "popular" ? "popular" : "recently-added")); }
    catch (error) { setMessage(errorText(error)); }
    finally { setLoading(false); }
  }

  async function runSearch(value: string) {
    if (!value.trim()) { await loadCollection(feed); return; }
    setLoading(true); setMessage("");
    try { setApps(await searchApps(value.trim())); }
    catch (error) { setMessage(errorText(error)); }
    finally { setLoading(false); }
  }

  async function openDetails(app: StoreApp) {
    setSelected(app); setMessage("");
    try { setSelected(await getAppDetails(app)); }
    catch (error) { setMessage(errorText(error)); }
  }

  async function startInstall(app: StoreApp | null = selected) {
    if (!app || busyRef) return;
    const token = ++operationToken.current;
    const operationId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    operationIdRef.current = operationId;
    operationKindRef.current = "install";
    setBusyRef(app.ref); setProgress("Preparing installation…"); setPercent(null); setMessage("");
    try {
      const icon = await fetchIconBase64(app.icon);
      await installApp(app.ref, operationId, icon);
      if (operationIdRef.current !== operationId) return;
      setProgress("Downloading and installing…");
      setTimeout(() => {
        if (operationToken.current !== token) return;
        operationIdRef.current = "";
        setBusyRef(""); setProgress(""); setPercent(null); setMessage("The operation timed out. Check Installed before trying again.");
      }, 10 * 60 * 1000);
    } catch (error) { operationIdRef.current = ""; setProgress(""); setPercent(null); setMessage(errorText(error)); setBusyRef(""); }
  }

  async function startUninstall() {
    if (!selected || busyRef) return;
    const token = ++operationToken.current;
    const operationId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    operationIdRef.current = operationId;
    operationKindRef.current = "uninstall";
    setBusyRef(selected.ref); setProgress("Preparing removal…"); setPercent(null); setMessage("");
    try {
      await uninstallApp(selected.ref, operationId);
      if (operationIdRef.current !== operationId) return;
      setProgress("Removing app…");
      setTimeout(() => {
        if (operationToken.current !== token) return;
        operationIdRef.current = "";
        setBusyRef(""); setProgress(""); setPercent(null); setMessage("The operation timed out. Check Installed before trying again.");
      }, 10 * 60 * 1000);
    } catch (error) { operationIdRef.current = ""; setProgress(""); setPercent(null); setMessage(errorText(error)); setBusyRef(""); }
  }

  useEffect(() => {
    const timer = setTimeout(() => { void loadCollection("popular"); void refreshInstalled(); }, 0);
    return () => clearTimeout(timer);
  }, []);
  useEffect(() => {
    const subscription = MatonOS.subscribe("flatpak", "progress", (event) => {
      let payload: ProgressEvent;
      try { payload = JSON.parse(event.json) as ProgressEvent; } catch { return; }
      if (payload.phase === "complete") {
        if (!payload.result?.operationId || payload.result.operationId !== operationIdRef.current) return;
        operationToken.current++;
        operationIdRef.current = "";
        const completed = payload.result;
        setBusyRef(""); setProgress(""); setPercent(null);
        if (!completed?.ok) setMessage(completed?.error || completed?.output?.trim() || "The Flatpak operation did not complete.");
        else setMessage("Operation complete.");
        void refreshInstalled();
        return;
      }
      if (!operationIdRef.current) return;
      const eventPercent = typeof payload.percent === "number" ? payload.percent : null;
      setPercent(eventPercent);
      setProgress(`${payload.line || "Working…"}${eventPercent == null ? "" : ` ${eventPercent}%`}`);
    });
    return () => subscription.remove();
  }, []);

  async function selectFeed(next: Feed) {
    setFeed(next); await loadCollection(next);
  }

  async function selectPage(next: Page) {
    setPage(next); setSelected(null); setMessage("");
    if (next === "installed") await refreshInstalled();
    if (next === "browse") await loadCollection(feed);
  }

  async function launchApp(app: StoreApp) {
    setMessage("");
    try {
      await runApp(app.id);
      setMessage(`Opening ${app.name}…`);
    } catch (error) { setMessage(errorText(error)); }
  }

  const renderApp = ({ item }: { item: StoreApp }) => (
    <Host matchContents={{ vertical: true }} useViewportSizeMeasurement style={{ width: "100%", minHeight: 96 }} colorScheme="dark" seedColor="#79c75b">
      <AppTile app={item} installed={installedIds.has(item.id)} busy={Boolean(busyRef)}
        busyLabel={busyRef === item.ref ? busyButtonLabel(operationKindRef.current) : ""}
        progress={busyRef === item.ref ? progress : ""} percent={busyRef === item.ref ? percent : null}
        onOpenDetails={openDetails}
        onInstall={(app) => void startInstall(app)} onLaunch={(app) => void launchApp(app)} />
    </Host>
  );
  const keyApp = (app: StoreApp) => app.id;

  let content: ReactNode;
  if (selected) {
    const currentDetail = selected;
    content = <Host style={{ flex: 1 }} colorScheme="dark" seedColor="#79c75b"><DetailContent app={currentDetail} installed={installedIds.has(currentDetail.id)} busy={Boolean(busyRef)}
      busyLabel={busyButtonLabel(operationKindRef.current)} progress={busyRef === currentDetail.ref ? progress : ""} percent={busyRef === currentDetail.ref ? percent : null}
      onBack={() => setSelected(null)} onInstall={() => void startInstall(currentDetail)} onUninstall={() => void startUninstall()} /></Host>;
  } else if (page === "installed") {
    content = <View style={{ flex: 1 }}>
      <Host matchContents={{ vertical: true }} colorScheme="dark" seedColor="#79c75b"><Column>
      <Text style={{ typography: "headlineSmall" }} modifiers={[paddingAll(20)]}>Installed apps</Text>
      <Show when={loading}><Text modifiers={[paddingAll(20)]}>Loading installed apps…</Text></Show>
      <Show when={Boolean(installedError) && !loading}><Text modifiers={[paddingAll(20)]}>{installedError}</Text></Show>
      </Column></Host>
      <FlatList removeClippedSubviews={false} style={{ flex: 1 }} data={installedApps} renderItem={renderApp} keyExtractor={keyApp}
        ListEmptyComponent={!loading && !installedError ? <Host style={{ width: "100%", height: 64 }} colorScheme="dark"><Text modifiers={[paddingAll(20)]}>No installed apps are listed yet.</Text></Host> : null} />
    </View>;
  } else {
    content = <View style={{ flex: 1 }}>
      <Host matchContents={{ vertical: true }} colorScheme="dark" seedColor="#79c75b"><Column>
      <Text style={{ typography: "headlineSmall" }} modifiers={[padding(20, 18, 20, 0)]}>Explore Flathub</Text>
      <Row horizontalArrangement={{ spacedBy: 10 }} modifiers={[padding(16, 10, 16, 10)]}>
        <Button onClick={() => void selectFeed("popular")}><Text>Popular</Text></Button>
        <Button onClick={() => void selectFeed("new")}><Text>New</Text></Button>
      </Row>
      <OutlinedTextField onValueChange={(value) => void runSearch(value)} modifiers={[padding(16, 0, 16, 0)]}>
        <OutlinedTextField.Label>Search apps</OutlinedTextField.Label>
      </OutlinedTextField>
      </Column></Host>
      <FlatList removeClippedSubviews={false} style={{ flex: 1 }} data={apps} renderItem={renderApp} keyExtractor={keyApp}
        ListHeaderComponent={loading ? <Host style={{ width: "100%", height: 64 }} colorScheme="dark"><Text modifiers={[paddingAll(16)]}>Loading Flathub…</Text></Host> : null}
        ListEmptyComponent={!loading ? <Host style={{ width: "100%", height: 64 }} colorScheme="dark"><Text modifiers={[paddingAll(16)]}>{message || "No apps found."}</Text></Host> : null} />
    </View>;
  }

  return (
    <View style={{ flex: 1, backgroundColor: themeColors.background }}>
      <Host matchContents={{ vertical: true }} colorScheme="dark" seedColor="#79c75b"><Column>
        <Row horizontalArrangement={{ spacedBy: 12 }} modifiers={[paddingAll(14)]}>
          <Text style={{ typography: "titleLarge" }} modifiers={[weight(1)]}>Software Center</Text>
          <Button onClick={() => void selectPage("browse")}><Text>Browse</Text></Button>
          <Button onClick={() => void selectPage("installed")}><Text>Installed</Text></Button>
        </Row>
        <Show when={Boolean(busyRef)}>
          <Column modifiers={[padding(14, 0, 14, 14)]}>
            <LinearProgressIndicator progress={percentToRatio(percent)} />
            <Show when={Boolean(progress)}><Text style={{ typography: "bodyMedium" }} maxLines={2} modifiers={[padding(6, 0, 0, 0)]}>{progress}</Text></Show>
          </Column>
        </Show>
        <Show when={Boolean(message)}><Text style={{ typography: "bodyMedium" }} modifiers={[padding(18, 0, 18, 8)]}>{message}</Text></Show>
      </Column></Host>
      {content}
    </View>
  );
}
