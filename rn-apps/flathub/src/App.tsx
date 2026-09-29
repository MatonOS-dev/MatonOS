import { useEffect, useMemo, useState, type ReactNode } from "react";
import { Image as ComposeImage, Button, Column, Host, LazyColumn, OutlinedTextField, Row, Text } from "@expo/ui/jetpack-compose";
import { fillMaxSize, height, padding, paddingAll, size, weight, width } from "@expo/ui/jetpack-compose/modifiers";
import { getAppById, getAppDetails, getCollection, searchApps, type StoreApp } from "./FlathubApi";
import { getInstalledRefs, installApp, uninstallApp, type ProgressEvent } from "./FlatpakBridge";
import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";

type Page = "browse" | "installed";
type Feed = "popular" | "new";

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : "Something went wrong.";
}

function AppTile({ app, installed, onOpen }: { app: StoreApp; installed: boolean; onOpen: (app: StoreApp) => void }) {
  return (
    <Row verticalAlignment="center" modifiers={[paddingAll(12)]}>
      {app.icon ? <ComposeImage source={{ uri: app.icon }} contentDescription={`${app.name} icon`} modifiers={[size(52, 52)]} /> : null}
      <Column modifiers={[weight(1), padding(12, 0, 12, 0)]}>
        <Text style={{ typography: "titleMedium" }}>{app.name}</Text>
        <Text style={{ typography: "bodyMedium" }} maxLines={2}>{app.summary}</Text>
      </Column>
      <Button onClick={() => onOpen(app)}><Text>{installed ? "Open" : "View"}</Text></Button>
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
        {app.icon ? <ComposeImage source={{ uri: app.icon }} contentDescription={`${app.name} icon`} modifiers={[size(84, 84)]} /> : null}
        <Column modifiers={[padding(18, 0, 18, 0)]}>
          <Text style={{ typography: "headlineSmall" }}>{app.name}</Text>
          <Text style={{ typography: "bodyLarge" }}>{app.summary}</Text>
          <Text style={{ typography: "bodySmall" }}>{app.developer}</Text>
        </Column>
      </Row>
      <Row horizontalArrangement={{ spacedBy: 10 }}>
        <Button enabled={!busy} onClick={action}><Text>{actionLabel}</Text></Button>
        {progress ? <Text style={{ typography: "bodyMedium" }}>{progress}</Text> : null}
      </Row>
      {app.screenshots.length > 0 ? <Text style={{ typography: "titleMedium" }}>Screenshots</Text> : null}
      {app.screenshots.length > 0 ? <LazyColumn horizontalAlignment="start" verticalArrangement={{ spacedBy: 10 }}>{screenshotTiles(app)}</LazyColumn> : null}
      <Text style={{ typography: "titleMedium" }}>Permissions</Text>
      <Text style={{ typography: "bodyMedium" }}>{permissionText}</Text>
      <Text style={{ typography: "titleMedium" }}>About this app</Text>
      <Text style={{ typography: "bodyMedium" }}>{app.description.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim() || app.summary}</Text>
      <Text style={{ typography: "bodySmall" }}>{app.id}</Text>
    </LazyColumn>
  );
}

function appTiles(apps: StoreApp[], installed: Set<string>, onOpen: (app: StoreApp) => void): ReactNode[] {
  return apps.map((app) => <AppTile key={app.id} app={app} installed={installed.has(app.id)} onOpen={onOpen} />);
}

function screenshotTiles(app: StoreApp): ReactNode[] {
  return app.screenshots.map((src, index) => <ComposeImage key={`${app.id}-${index}`} source={{ uri: src }} contentDescription={`${app.name} screenshot`} modifiers={[width(620), height(349)]} />);
}

export default function App() {
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

  async function startInstall() {
    if (!selected) return;
    setBusyRef(selected.ref); setProgress("Preparing installation…"); setMessage("");
    try {
      await installApp(selected.ref);
      setProgress("Downloading and installing…");
    } catch (error) { setProgress(""); setMessage(errorText(error)); setBusyRef(""); }
  }

  async function startUninstall() {
    if (!selected) return;
    setBusyRef(selected.ref); setProgress("Preparing removal…"); setMessage("");
    try {
      await uninstallApp(selected.ref);
      setProgress("Removing app and its data…");
    } catch (error) { setProgress(""); setMessage(errorText(error)); setBusyRef(""); }
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
        const completed = payload.result;
        setBusyRef(""); setProgress("");
        if (!completed?.ok) setMessage(completed?.error || "The Flatpak operation did not complete.");
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

  let content: ReactNode;
  if (selected) {
    const currentDetail = selected;
    content = <DetailContent app={currentDetail} installed={installedIds.has(currentDetail.id)} busy={Boolean(busyRef)} progress={busyRef ? progress : ""}
      onBack={() => setSelected(null)} onInstall={() => void startInstall()} onUninstall={() => void startUninstall()} />;
  } else if (page === "installed") {
    const tiles = appTiles(installedApps, installedIds, (app) => void openDetails(app));
    content = <Column modifiers={[weight(1)]}>
      <Text style={{ typography: "headlineSmall" }} modifiers={[paddingAll(20)]}>Installed apps</Text>
      {loading ? <Text modifiers={[paddingAll(20)]}>Loading installed apps…</Text> : null}
      {installedError && !loading ? <Text modifiers={[paddingAll(20)]}>{installedError}</Text> : null}
      {installedApps.length === 0 && !installedError && !loading ? <Text modifiers={[paddingAll(20)]}>No installed apps are listed yet.</Text> : null}
      <LazyColumn verticalArrangement={{ spacedBy: 4 }} contentPadding={{ bottom: 20 }}>{tiles}</LazyColumn>
    </Column>;
  } else {
    const tiles = appTiles(apps, installedIds, (app) => void openDetails(app));
    content = <Column modifiers={[weight(1)]}>
      <Text style={{ typography: "headlineSmall" }} modifiers={[padding(20, 18, 20, 0)]}>Explore Flathub</Text>
      <Row horizontalArrangement={{ spacedBy: 10 }} modifiers={[padding(16, 10, 16, 10)]}>
        <Button onClick={() => void selectFeed("popular")}><Text>Popular</Text></Button>
        <Button onClick={() => void selectFeed("new")}><Text>New</Text></Button>
      </Row>
      <OutlinedTextField onValueChange={(value) => void runSearch(value)} modifiers={[padding(16, 0, 16, 0)]}>
        <OutlinedTextField.Label>Search apps</OutlinedTextField.Label>
      </OutlinedTextField>
      {loading ? <Text modifiers={[paddingAll(16)]}>Loading Flathub…</Text> : null}
      <LazyColumn verticalArrangement={{ spacedBy: 4 }} contentPadding={{ bottom: 20 }}>{tiles}</LazyColumn>
    </Column>;
  }

  return (
    <Host style={{ flex: 1 }} colorScheme="dark" seedColor="#79c75b">
      <Column modifiers={[fillMaxSize()]}>
        <Row horizontalArrangement={{ spacedBy: 12 }} modifiers={[paddingAll(14)]}>
          <Text style={{ typography: "titleLarge" }} modifiers={[weight(1)]}>Flathub Store</Text>
          <Button onClick={() => void selectPage("browse")}><Text>Browse</Text></Button>
          <Button onClick={() => void selectPage("installed")}><Text>Installed</Text></Button>
        </Row>
        {message ? <Text style={{ typography: "bodyMedium" }} modifiers={[padding(18, 0, 18, 8)]}>{message}</Text> : null}
        {content}
      </Column>
    </Host>
  );
}
