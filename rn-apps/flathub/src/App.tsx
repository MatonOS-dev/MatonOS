import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { FlatList, Linking, View } from "react-native";
import { Image as ComposeImage, Button, Column, Host, LazyColumn, LinearProgressIndicator, OutlinedTextField, Row, Text, RNHostView, useNativeState } from "@expo/ui/jetpack-compose";
import { useMaterialColors } from "@expo/ui/jetpack-compose";
import { clickable, height, padding, paddingAll, size, weight, width } from "@expo/ui/jetpack-compose/modifiers";
import { getCollection, type StoreApp } from "./FlathubApi";
import { MatonOS } from "../modules/matonos-flathub/src/MatonOS";
import { runApp } from "./FlatpakBridge";
import { FlathubSource } from "./sources/FlathubSource";
import { FdroidSource } from "./sources/FdroidSource";
import type { Source, SourceProgress } from "./sources/Source";

type Page = "browse" | "installed" | "updates";
type Feed = "popular" | "new";
const sources: Source[] = [FlathubSource, FdroidSource];

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

function SourceBadge({ app }: { app: StoreApp }) {
  return <Text style={{ typography: "labelSmall" }}>{app.source}</Text>;
}

function ProgressLine({ progress }: { progress: string }) {
  if (!progress) return null;
  return <Text style={{ typography: "bodyMedium" }} maxLines={2}>{progress}</Text>;
}

function busyButtonLabel(kind: "install" | "update" | "uninstall"): string {
  if (kind === "update") return "Updating…";
  return kind === "install" ? "Installing…" : "Working…";
}

function percentToRatio(percent: number | null): number | null {
  if (percent == null) return null;
  return Math.min(1, Math.max(0, percent / 100));
}

function progressLabel(phase: string, percent: number | null, bytesDone: number, bytesTotal: number): string {
  const fraction = percent == null ? "" : ` ${percent}%`;
  const size = bytesTotal > 0 ? ` · ${Math.floor(Math.max(0, bytesDone) / 1048576)} / ${Math.ceil(bytesTotal / 1048576)} MB` : "";
  return `${phase || "Working…"}${fraction}${size}`;
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

function AppTile({ app, installed, updateAvailable, busy, busyLabel, progress, percent, onOpenDetails, onInstall, onUninstall, onLaunch }: {
  app: StoreApp;
  installed: boolean;
  updateAvailable: boolean;
  busy: boolean;
  busyLabel: string;
  progress: string;
  percent: number | null;
  onOpenDetails: (app: StoreApp) => void;
  onInstall: (app: StoreApp) => void;
  onUninstall: (app: StoreApp) => void;
  onLaunch: (app: StoreApp) => void;
}) {
  const isBusy = busy && Boolean(progress);
  const buttonLabel = isBusy ? (busyLabel || "Working…") : updateAvailable ? "Update" : installed ? app.source === "Flathub" ? "Open" : "Uninstall" : "Install";
  const buttonAction = updateAvailable ? () => onInstall(app) : installed ? app.source === "Flathub" ? () => onLaunch(app) : () => onUninstall(app) : () => onInstall(app);
  return (
    <Column modifiers={[clickable(() => onOpenDetails(app))]}>
      <Row verticalAlignment="center" modifiers={[paddingAll(12)]}>
        <AppIcon app={app} imageSize={52} />
        <Column modifiers={[weight(1), padding(12, 0, 12, 0)]}>
          <Text style={{ typography: "titleMedium" }}>{app.name}</Text>
          <SourceBadge app={app} />
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

function DetailContent({ app, installed, updateAvailable, busy, busyLabel, progress, percent, onBack, onInstall, onUninstall }: {
  app: StoreApp; installed: boolean; updateAvailable: boolean; busy: boolean; busyLabel: string; progress: string; percent: number | null; onBack: () => void; onInstall: () => void; onUninstall: () => void;
}) {
  const permissionText = app.permissions.length ? app.permissions.join(" · ") : "Permission details are not included for this app's source.";
  const actionLabel = busy ? (busyLabel || "Working…") : updateAvailable ? "Update" : installed ? "Uninstall" : "Install";
  const action = updateAvailable ? onInstall : installed ? onUninstall : onInstall;
  return (
    <LazyColumn contentPadding={{ start: 20, top: 20, end: 20, bottom: 20 }} verticalArrangement={{ spacedBy: 14 }}>
      <Button onClick={onBack}><Text>‹  Back to apps</Text></Button>
      <Row verticalAlignment="center">
        <AppIcon app={app} imageSize={84} />
        <Column modifiers={[padding(18, 0, 18, 0)]}>
          <Text style={{ typography: "headlineSmall" }}>{app.name}</Text>
          <SourceBadge app={app} />
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
      {app.source === "F-Droid" ? <>
        <Text style={{ typography: "titleMedium" }}>F-Droid package</Text>
        <Text style={{ typography: "bodyMedium" }}>Version {app.versionName} ({app.versionCode})</Text>
        <Text style={{ typography: "bodySmall" }}>{app.apkName}</Text>
        <Text style={{ typography: "bodySmall" }}>SHA-256: {app.apkSha256}</Text>
      </> : null}
      <Text style={{ typography: "bodySmall" }}>{app.id}</Text>
    </LazyColumn>
  );
}

export default function App() {
  const themeColors = useMaterialColors({ colorScheme: "dark", seedColor: "#79c75b" });
  const [page, setPage] = useState<Page>("browse");
  const [feed, setFeed] = useState<Feed>("popular");
  const [repoManager, setRepoManager] = useState(false);
  const [repositories, setRepositories] = useState<Array<{ url: string; fingerprint: string; official: boolean; broken: boolean }>>([]);
  const [repoUrl, setRepoUrl] = useState("");
  const [repoFingerprint, setRepoFingerprint] = useState("");
  const repoUrlInput = useNativeState("");
  const repoFingerprintInput = useNativeState("");
  const [repoHost, setRepoHost] = useState("");
  const [repoError, setRepoError] = useState("");
  const [repoChecking, setRepoChecking] = useState(false);
  const [apps, setApps] = useState<StoreApp[]>([]);
  const [installedApps, setInstalledApps] = useState<StoreApp[]>([]);
  const [updateApps, setUpdateApps] = useState<StoreApp[]>([]);
  const [installedError, setInstalledError] = useState("");
  const [selected, setSelected] = useState<StoreApp | null>(null);
  const [loading, setLoading] = useState(false);
  const [message, setMessage] = useState("");
  const [busyRef, setBusyRef] = useState("");
  const [progress, setProgress] = useState("");
  const [percent, setPercent] = useState<number | null>(null);
  const operationIdRef = useRef("");
  const [operationKind, setOperationKind] = useState<"install" | "update" | "uninstall">("install");
  const installedIds = useMemo(() => new Set(installedApps.map((app) => `${app.source}:${app.id}`)), [installedApps]);
  const updateIds = useMemo(() => new Set(updateApps.map((app) => `${app.source}:${app.id}`)), [updateApps]);

  async function refreshRepositories() {
    try { setRepositories(await MatonOS.fdroidRepositories()); } catch (error) { setRepoError(errorText(error)); }
  }

  const reviewRepository = useCallback(async (url: string, fingerprint: string) => {
    setRepoError(""); setRepoUrl(url); setRepoFingerprint(fingerprint); repoUrlInput.set(url); repoFingerprintInput.set(fingerprint);
    try { setRepoHost(await MatonOS.fdroidHostDisplay(url)); }
    catch (error) { setRepoHost(""); setRepoError(errorText(error)); }
  }, [repoUrlInput, repoFingerprintInput]);

  const handleRepositoryLink = useCallback(async (value: string) => {
    try {
      const link = new URL(value);
      if (link.protocol !== "fdroidrepos:") return;
      const url = link.searchParams.get("url") || `https://${link.host}${link.pathname}`.replace(/\/$/, "");
      setRepoManager(true); setSelected(null);
      await reviewRepository(url, link.searchParams.get("fingerprint") || "");
    } catch { setRepoError("This F-Droid repository link is invalid."); }
  }, [reviewRepository]);

  async function saveRepository() {
    if (!repoHost || repoChecking) return;
    setRepoChecking(true); setRepoError("");
    try {
      await MatonOS.fdroidAddRepository(repoUrl, repoFingerprint);
      const status = await MatonOS.fdroidVerifyRepository(repoUrl);
      await refreshRepositories();
      setRepoError(status.ok ? "Repository verified and added." : `Repository added as broken: ${status.error}`);
      if (status.ok) { setRepoUrl(""); setRepoFingerprint(""); setRepoHost(""); repoUrlInput.set(""); repoFingerprintInput.set(""); }
    } catch (error) { setRepoError(errorText(error)); }
    finally { setRepoChecking(false); }
  }

  async function removeRepository(repository: string) {
    try { await MatonOS.fdroidRemoveRepository(repository); await refreshRepositories(); await runSearch(""); }
    catch (error) { setRepoError(errorText(error)); }
  }

  async function refreshInstalled() {
    setInstalledError("");
    try {
      const entries = (await Promise.all(sources.map((source) => source.installed()))).flat();
      setInstalledApps(entries);
    } catch (error) {
      setInstalledApps([]); setInstalledError(errorText(error));
    }
  }

  async function refreshUpdates() {
    try {
      const entries = (await Promise.all(sources.map((source) => source.updates()))).flat();
      setUpdateApps(entries);
    } catch { setUpdateApps([]); }
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
    try {
      const results = await Promise.all(sources.map((source) => source.search(value.trim())));
      setApps(results.flat());
    }
    catch (error) { setMessage(errorText(error)); }
    finally { setLoading(false); }
  }

  async function openDetails(app: StoreApp) {
    setSelected(app); setMessage("");
    try { setSelected(await sources.find((source) => source.name === app.source)!.details(app)); }
    catch (error) { setMessage(errorText(error)); }
  }

  async function startInstall(app: StoreApp | null = selected) {
    if (!app || busyRef) return;
    const operationId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    operationIdRef.current = operationId;
    setOperationKind(installedIds.has(`${app.source}:${app.id}`) ? "update" : "install");
    setBusyRef(app.ref); setProgress("Preparing installation…"); setPercent(null); setMessage("");
    try {
      await sources.find((source) => source.name === app.source)!.install(app, operationId);
      if (operationIdRef.current !== operationId) return;
      setProgress("Downloading and installing…");
    } catch (error) { operationIdRef.current = ""; setProgress(""); setPercent(null); setMessage(errorText(error)); setBusyRef(""); }
  }

  async function startUninstall(app: StoreApp | null = selected) {
    if (!app || busyRef) return;
    const operationId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    operationIdRef.current = operationId;
    setOperationKind("uninstall");
    setBusyRef(app.ref); setProgress("Preparing removal…"); setPercent(null); setMessage("");
    try {
      await sources.find((source) => source.name === app.source)!.uninstall(app, operationId);
      if (operationIdRef.current !== operationId) return;
      setProgress("Removing app…");
    } catch (error) { operationIdRef.current = ""; setProgress(""); setPercent(null); setMessage(errorText(error)); setBusyRef(""); }
  }

  useEffect(() => {
    const timer = setTimeout(() => { void loadCollection("popular"); void refreshInstalled(); void refreshUpdates(); void refreshRepositories(); }, 0);
    const initialLink = Linking.getInitialURL().then((url) => { if (url) void handleRepositoryLink(url); });
    const subscription = Linking.addEventListener("url", (event) => void handleRepositoryLink(event.url));
    return () => { clearTimeout(timer); subscription.remove(); void initialLink; };
  }, [handleRepositoryLink]);
  useEffect(() => {
    const handleProgress = (payload: SourceProgress) => {
      if (payload.phase === "complete") {
        if (!payload.result?.operationId || payload.result.operationId !== operationIdRef.current) return;
        operationIdRef.current = "";
        const completed = payload.result;
        setBusyRef(""); setProgress(""); setPercent(null);
        if (!completed?.ok) setMessage(completed?.error || completed?.output?.trim() || "The operation did not complete.");
        else setMessage("Operation complete.");
        void refreshInstalled();
        void refreshUpdates();
        return;
      }
      if (payload.operationId !== operationIdRef.current) return;
      // The installer reports -1 for phases without a known fraction.
      const eventPercent = typeof payload.percent === "number" && payload.percent >= 0 ? payload.percent : null;
      setPercent(eventPercent);
      const done = typeof payload.bytesDone === "number" ? payload.bytesDone : 0;
      const total = typeof payload.bytesTotal === "number" ? payload.bytesTotal : 0;
      setProgress(progressLabel(payload.phase || "Working…", eventPercent, done, total));
    };
    const subscriptions = sources.map((source) => source.progress(handleProgress));
    return () => subscriptions.forEach((subscription) => subscription.remove());
  }, []);

  async function selectFeed(next: Feed) {
    setFeed(next); await loadCollection(next);
  }

  async function selectPage(next: Page) {
    setPage(next); setSelected(null); setMessage("");
    if (next === "installed") await refreshInstalled();
    if (next === "updates") await refreshUpdates();
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
      <AppTile app={item} installed={installedIds.has(`${item.source}:${item.id}`)} updateAvailable={updateIds.has(`${item.source}:${item.id}`)} busy={Boolean(busyRef)}
        busyLabel={busyRef === item.ref ? busyButtonLabel(operationKind) : ""}
        progress={busyRef === item.ref ? progress : ""} percent={busyRef === item.ref ? percent : null}
        onOpenDetails={openDetails}
        onInstall={(app) => void startInstall(app)} onUninstall={(app) => void startUninstall(app)} onLaunch={(app) => void launchApp(app)} />
    </Host>
  );
  const keyApp = (app: StoreApp) => `${app.source}:${app.id}`;

  let content: ReactNode;
  if (repoManager) {
    content = <Host style={{ flex: 1 }} colorScheme="dark" seedColor="#79c75b"><LazyColumn contentPadding={{ start: 20, top: 20, end: 20, bottom: 20 }} verticalArrangement={{ spacedBy: 12 }}>
      <Button onClick={() => { setRepoManager(false); setRepoError(""); }}><Text>‹  Back to apps</Text></Button>
      <Text style={{ typography: "headlineSmall" }}>F-Droid repositories</Text>
      <Text style={{ typography: "bodyMedium" }}>The Software Center verifies each repository's signed index using the fingerprint you confirm.</Text>
      {repositories.map((repository) => <Column key={repository.url} modifiers={[paddingAll(10)]}>
        <Text style={{ typography: "titleMedium" }}>{repository.official ? "F-Droid (built in)" : repository.url}</Text>
        <Text style={{ typography: "bodySmall" }}>SHA-256: {repository.fingerprint}</Text>
        <Text style={{ typography: "bodySmall" }}>{repository.broken ? "Broken: index verification or download failed." : "Verified repository"}</Text>
        {!repository.official ? <Button onClick={() => void removeRepository(repository.url)}><Text>Remove repository</Text></Button> : null}
      </Column>)}
      <OutlinedTextField value={repoUrlInput} onValueChange={(value) => { setRepoUrl(value); repoUrlInput.set(value); }}><OutlinedTextField.Label>HTTPS repository URL</OutlinedTextField.Label></OutlinedTextField>
      <OutlinedTextField value={repoFingerprintInput} onValueChange={(value) => { setRepoFingerprint(value); repoFingerprintInput.set(value); }}><OutlinedTextField.Label>Signing fingerprint (SHA-256)</OutlinedTextField.Label></OutlinedTextField>
      <Button onClick={() => void reviewRepository(repoUrl, repoFingerprint)}><Text>Review repository</Text></Button>
      {repoHost ? <Column modifiers={[paddingAll(12)]}>
        <Text style={{ typography: "titleMedium" }}>Trust repository at {repoHost}?</Text>
        <Text style={{ typography: "bodySmall" }}>Fingerprint: {repoFingerprint.replace(/(.{2})/g, "$1:").replace(/:$/, "").toUpperCase()}</Text>
        <Text style={{ typography: "bodySmall" }}>Review the host and fingerprint before trusting this repository.</Text>
        <Button enabled={!repoChecking} onClick={() => void saveRepository()}><Text>{repoChecking ? "Checking…" : "Trust and add"}</Text></Button>
      </Column> : null}
      <Show when={Boolean(repoError)}><Text style={{ typography: "bodyMedium" }}>{repoError}</Text></Show>
    </LazyColumn></Host>;
  } else if (selected) {
    const currentDetail = selected;
    content = <Host style={{ flex: 1 }} colorScheme="dark" seedColor="#79c75b"><DetailContent app={currentDetail} installed={installedIds.has(`${currentDetail.source}:${currentDetail.id}`)} updateAvailable={updateIds.has(`${currentDetail.source}:${currentDetail.id}`)} busy={Boolean(busyRef)}
      busyLabel={busyButtonLabel(operationKind)} progress={busyRef === currentDetail.ref ? progress : ""} percent={busyRef === currentDetail.ref ? percent : null}
      onBack={() => setSelected(null)} onInstall={() => void startInstall(currentDetail)} onUninstall={() => void startUninstall()} /></Host>;
  } else if (page === "installed" || page === "updates") {
    const entries = page === "installed" ? installedApps : updateApps;
    const heading = page === "installed" ? "Installed apps" : "Updates";
    content = <View style={{ flex: 1 }}>
      <Host matchContents={{ vertical: true }} colorScheme="dark" seedColor="#79c75b"><Column>
      <Text style={{ typography: "headlineSmall" }} modifiers={[paddingAll(20)]}>{heading}</Text>
      <Show when={loading}><Text modifiers={[paddingAll(20)]}>Loading {page === "installed" ? "installed apps" : "updates"}…</Text></Show>
      <Show when={Boolean(installedError) && !loading}><Text modifiers={[paddingAll(20)]}>{installedError}</Text></Show>
      </Column></Host>
      <FlatList removeClippedSubviews={false} style={{ flex: 1 }} data={entries} renderItem={renderApp} keyExtractor={keyApp}
        ListEmptyComponent={!loading && !installedError ? <Host style={{ width: "100%", height: 64 }} colorScheme="dark"><Text modifiers={[paddingAll(20)]}>{page === "installed" ? "No installed apps are listed yet." : "No updates are available."}</Text></Host> : null} />
    </View>;
  } else {
    content = <View style={{ flex: 1 }}>
      <Host matchContents={{ vertical: true }} colorScheme="dark" seedColor="#79c75b"><Column>
      <Text style={{ typography: "headlineSmall" }} modifiers={[padding(20, 18, 20, 0)]}>Explore apps</Text>
      <Row horizontalArrangement={{ spacedBy: 10 }} modifiers={[padding(16, 10, 16, 10)]}>
        <Button onClick={() => void selectFeed("popular")}><Text>Popular</Text></Button>
        <Button onClick={() => void selectFeed("new")}><Text>New</Text></Button>
      </Row>
      <OutlinedTextField onValueChange={(value) => void runSearch(value)} modifiers={[padding(16, 0, 16, 0)]}>
        <OutlinedTextField.Label>Search apps</OutlinedTextField.Label>
      </OutlinedTextField>
      </Column></Host>
      <FlatList removeClippedSubviews={false} style={{ flex: 1 }} data={apps} renderItem={renderApp} keyExtractor={keyApp}
        ListHeaderComponent={loading ? <Host style={{ width: "100%", height: 64 }} colorScheme="dark"><Text modifiers={[paddingAll(16)]}>Loading apps…</Text></Host> : null}
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
          <Button onClick={() => void selectPage("updates")}><Text>Updates</Text></Button>
          <Button onClick={() => { setRepoManager(true); setSelected(null); setRepoError(""); void refreshRepositories(); }}><Text>Repositories</Text></Button>
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
