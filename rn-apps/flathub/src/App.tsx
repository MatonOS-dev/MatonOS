import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { FlatList } from "react-native";
import { Image as ComposeImage, Button, Column, Host, LazyColumn, OutlinedTextField, Row, Text } from "@expo/ui/jetpack-compose";
import { useMaterialColors } from "@expo/ui/jetpack-compose";
import { background, clickable, fillMaxSize, height, padding, paddingAll, size, weight, width } from "@expo/ui/jetpack-compose/modifiers";
import { getAppById, getAppDetails, getCollection, searchApps, type StoreApp } from "./FlathubApi";
import { getInstalledRefs, installApp, runApp, uninstallApp, type ProgressEvent } from "./FlatpakBridge";
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
  return <Text style={{ typography: "bodyMedium" }}>{progress}</Text>;
}

function Screenshots({ app }: { app: StoreApp }) {
  if (app.screenshots.length === 0) return null;
  return (
    <>
      <Text style={{ typography: "titleMedium" }}>Screenshots</Text>
      <FlatList horizontal data={app.screenshots} keyExtractor={(_, index) => `${app.id}-${index}`}
        renderItem={({ item: src }) => <ComposeImage source={{ uri: src }} contentDescription={`${app.name} screenshot`} modifiers={[width(620), height(349)]} />} />
    </>
  );
}

function AppTile({ app, installed, busy, progress, onOpenDetails, onInstall, onLaunch }: {
  app: StoreApp;
  installed: boolean;
  busy: boolean;
  progress: string;
  onOpenDetails: (app: StoreApp) => void;
  onInstall: (app: StoreApp) => void;
  onLaunch: (app: StoreApp) => void;
}) {
  const isBusy = busy && Boolean(progress);
  const buttonLabel = isBusy ? progress : installed ? "Open" : "Install";
  const buttonAction = installed ? () => onLaunch(app) : () => onInstall(app);
  return (
    <Row verticalAlignment="center" modifiers={[paddingAll(12), clickable(() => onOpenDetails(app))]}>
      <AppIcon app={app} imageSize={52} />
      <Column modifiers={[weight(1), padding(12, 0, 12, 0)]}>
        <Text style={{ typography: "titleMedium" }}>{app.name}</Text>
        <Text style={{ typography: "bodyMedium" }} maxLines={2}>{app.summary}</Text>
      </Column>
      <Button enabled={!busy} onClick={buttonAction}><Text>{buttonLabel}</Text></Button>
    </Row>
  );
}

function DetailContent({ app, installed, busy, progress, onBack, onInstall, onUninstall }: {
  app: StoreApp; installed: boolean; busy: boolean; progress: string; onBack: () => void; onInstall: () => void; onUninstall: () => void;
}) {
  const permissionText = app.permissions.length ? app.permissions.join(" · ") : "Permission details are not included for this app by the Flathub API.";
  const actionLabel = installed ? "Uninstall" : "Install";
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
      <Row horizontalArrangement={{ spacedBy: 10 }}>
        <Button enabled={!busy} onClick={action}><Text>{actionLabel}</Text></Button>
        <ProgressLine progress={progress} />
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
  const operationToken = useRef(0);
  const operationIdRef = useRef("");
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
    setBusyRef(app.ref); setProgress("Preparing installation…"); setMessage("");
    try {
      await installApp(app.ref, operationId);
      if (operationIdRef.current !== operationId) return;
      setProgress("Downloading and installing…");
      setTimeout(() => {
        if (operationToken.current !== token) return;
        operationIdRef.current = "";
        setBusyRef(""); setProgress(""); setMessage("The operation timed out. Check Installed before trying again.");
      }, 10 * 60 * 1000);
    } catch (error) { operationIdRef.current = ""; setProgress(""); setMessage(errorText(error)); setBusyRef(""); }
  }

  async function startUninstall() {
    if (!selected || busyRef) return;
    const token = ++operationToken.current;
    const operationId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    operationIdRef.current = operationId;
    setBusyRef(selected.ref); setProgress("Preparing removal…"); setMessage("");
    try {
      await uninstallApp(selected.ref, operationId);
      if (operationIdRef.current !== operationId) return;
      setProgress("Removing app…");
      setTimeout(() => {
        if (operationToken.current !== token) return;
        operationIdRef.current = "";
        setBusyRef(""); setProgress(""); setMessage("The operation timed out. Check Installed before trying again.");
      }, 10 * 60 * 1000);
    } catch (error) { operationIdRef.current = ""; setProgress(""); setMessage(errorText(error)); setBusyRef(""); }
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
        setBusyRef(""); setProgress("");
        if (!completed?.ok) setMessage(completed?.error || completed?.output?.trim() || "The Flatpak operation did not complete.");
        else setMessage("Operation complete.");
        void refreshInstalled();
        return;
      }
      const percent = typeof payload.percent === "number" ? ` ${payload.percent}%` : "";
      setProgress(`${payload.line || "Working…"}${percent}`);
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
    <AppTile app={item} installed={installedIds.has(item.id)} busy={Boolean(busyRef)}
      progress={busyRef === item.ref ? progress : ""} onOpenDetails={openDetails}
      onInstall={(app) => void startInstall(app)} onLaunch={(app) => void launchApp(app)} />
  );
  const keyApp = (app: StoreApp) => app.id;

  let content: ReactNode;
  if (selected) {
    const currentDetail = selected;
    content = <DetailContent app={currentDetail} installed={installedIds.has(currentDetail.id)} busy={Boolean(busyRef)} progress={busyRef === currentDetail.ref ? progress : ""}
      onBack={() => setSelected(null)} onInstall={() => void startInstall(currentDetail)} onUninstall={() => void startUninstall()} />;
  } else if (page === "installed") {
    content = <Column modifiers={[weight(1)]}>
      <Text style={{ typography: "headlineSmall" }} modifiers={[paddingAll(20)]}>Installed apps</Text>
      <Show when={loading}><Text modifiers={[paddingAll(20)]}>Loading installed apps…</Text></Show>
      <Show when={Boolean(installedError) && !loading}><Text modifiers={[paddingAll(20)]}>{installedError}</Text></Show>
      <FlatList style={{ flex: 1 }} data={installedApps} renderItem={renderApp} keyExtractor={keyApp}
        ListEmptyComponent={!loading && !installedError ? <Text modifiers={[paddingAll(20)]}>No installed apps are listed yet.</Text> : null} />
    </Column>;
  } else {
    content = <Column modifiers={[weight(1)]}>
      <Text style={{ typography: "headlineSmall" }} modifiers={[padding(20, 18, 20, 0)]}>Explore Flathub</Text>
      <Row horizontalArrangement={{ spacedBy: 10 }} modifiers={[padding(16, 10, 16, 10)]}>
        <Button onClick={() => void selectFeed("popular")}><Text>Popular</Text></Button>
        <Button onClick={() => void selectFeed("new")}><Text>New</Text></Button>
      </Row>
      <OutlinedTextField onValueChange={(value) => void runSearch(value)} modifiers={[padding(16, 0, 16, 0)]}>
        <OutlinedTextField.Label>Search apps</OutlinedTextField.Label>
      </OutlinedTextField>
      <FlatList style={{ flex: 1 }} data={apps} renderItem={renderApp} keyExtractor={keyApp}
        ListHeaderComponent={loading ? <Text modifiers={[paddingAll(16)]}>Loading Flathub…</Text> : null}
        ListEmptyComponent={!loading ? <Text modifiers={[paddingAll(16)]}>{message || "No apps found."}</Text> : null} />
    </Column>;
  }

  return (
    <Host style={{ flex: 1 }} colorScheme="dark" seedColor="#79c75b">
      <Column modifiers={[fillMaxSize(), background(themeColors.background)]}>
        <Row horizontalArrangement={{ spacedBy: 12 }} modifiers={[paddingAll(14)]}>
          <Text style={{ typography: "titleLarge" }} modifiers={[weight(1)]}>Software Center</Text>
          <Button onClick={() => void selectPage("browse")}><Text>Browse</Text></Button>
          <Button onClick={() => void selectPage("installed")}><Text>Installed</Text></Button>
        </Row>
        <Show when={Boolean(message)}><Text style={{ typography: "bodyMedium" }} modifiers={[padding(18, 0, 18, 8)]}>{message}</Text></Show>
        {content}
      </Column>
    </Host>
  );
}
