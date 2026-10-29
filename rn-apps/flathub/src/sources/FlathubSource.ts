import { getAppDetails, searchApps, type StoreApp as FlathubApp } from "../FlathubApi";
import { getInstalledRefs, installApp, uninstallApp, type ProgressEvent } from "../FlatpakBridge";
import { MatonOS } from "../../modules/matonos-flathub/src/MatonOS";
import type { Source, StoreApp } from "./Source";

function fromFlathub(app: FlathubApp): StoreApp {
  return { ...app, source: "Flathub" };
}

export const FlathubSource: Source = {
  name: "Flathub",
  async search(query) { return (await searchApps(query)).map(fromFlathub); },
  async details(app) { return fromFlathub(await getAppDetails(app)); },
  async installed() {
    const refs = await getInstalledRefs();
    return Promise.all(refs.map(async (ref) => {
      const id = ref.split("/")[1];
      try { return fromFlathub(await getAppDetails({ id, name: id, summary: "", icon: "", developer: "", ref, description: "", screenshots: [], permissions: [], source: "Flathub" })); }
      catch { return { id, name: id, summary: "Installed Flatpak", icon: "", developer: "", ref, description: "", screenshots: [], permissions: [], source: "Flathub" as const }; }
    }));
  },
  async updates() { return []; },
  async install(app, operationId) { await installApp(app.ref, operationId); },
  async uninstall(app, operationId) { await uninstallApp(app.ref, operationId); },
  progress(listener) {
    const subscription = MatonOS.addStoreListener((event: ProgressEvent) => {
      if (event.source !== "F-Droid") listener(event);
    });
    return subscription;
  },
};
