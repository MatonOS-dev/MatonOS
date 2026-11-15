import { MatonOS, type SourcePlugin } from "../../modules/matonos-flathub/src/MatonOS";
import type { Review, Source, SourceProgress, StoreApp } from "./Source";

/** Contract version this store speaks (SOURCE-PLUGINS.md). */
const CONTRACT_VERSION = 1;

/** Plugin JSON: details fields are optional on listings. */
type PluginApp = Omit<StoreApp, "source" | "ref" | "description" | "screenshots" | "permissions">
  & Partial<Pick<StoreApp, "description" | "screenshots" | "permissions">>;

function parse<T>(plugin: string, call: string, json: string): T {
  try {
    return JSON.parse(json) as T;
  } catch (error) {
    throw new Error(`${plugin}: ${call} returned invalid JSON`, { cause: error });
  }
}

function toApp(plugin: string, app: PluginApp): StoreApp {
  return { ...app, description: app.description ?? "", screenshots: app.screenshots ?? [], permissions: app.permissions ?? [], source: plugin, ref: app.id };
}

/** Wraps one enabled plugin app as a Source. Data is the plugin's JSON, stamped with its package as source. */
export function pluginSource(plugin: SourcePlugin): Source {
  const pkg = plugin.package;
  const list = async (call: string, json: Promise<string>) => parse<PluginApp[]>(pkg, call, await json).map((app) => toApp(pkg, app));
  return {
    name: pkg,
    search: (term) => list("search", MatonOS.sourceSearch(pkg, term)),
    async details(app) { return toApp(pkg, parse<PluginApp>(pkg, "details", await MatonOS.sourceDetails(pkg, app.id))); },
    async reviews(app) {
      return parse<Array<Omit<Review, "date"> & { date: string }>>(pkg, "reviews", await MatonOS.sourceReviews(pkg, app.id))
        .map((review) => ({ ...review, date: new Date(review.date) }));
    },
    installed: () => list("installed", MatonOS.sourceInstalled(pkg)),
    updates: () => list("updates", MatonOS.sourceUpdates(pkg)),
    install: (app, operationId) => MatonOS.sourceInstall(pkg, app.id, operationId),
    uninstall: (app, operationId) => MatonOS.sourceUninstall(pkg, app.packageName ?? app.id, operationId),
    progress(listener: (event: SourceProgress) => void) {
      return MatonOS.addStoreListener((event) => { if (event.source === pkg) listener(event); });
    },
  };
}

/** Enabled plugins that speak this store's contract version. A plugin that fails is logged and skipped. */
export async function pluginSources(): Promise<Source[]> {
  const plugins = (await MatonOS.sourcePlugins()).filter((plugin) => plugin.enabled);
  const checked = await Promise.all(plugins.map(async (plugin) => {
    try {
      const info = parse<{ version?: number }>(plugin.package, "info", await MatonOS.sourceInfo(plugin.package));
      if (info.version !== CONTRACT_VERSION) throw new Error(`speaks contract ${info.version}, store speaks ${CONTRACT_VERSION}`);
      return pluginSource(plugin);
    } catch (error) {
      console.error(`Store source ${plugin.package} unusable`, error);
      return null;
    }
  }));
  return checked.filter((source): source is Source => source !== null);
}
