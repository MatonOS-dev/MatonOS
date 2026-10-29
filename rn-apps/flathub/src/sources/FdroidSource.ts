import { MatonOS } from "../../modules/matonos-flathub/src/MatonOS";
import type { Source, SourceProgress } from "./Source";

export const FdroidSource: Source = {
  name: "F-Droid",
  async search(query) {
    const repositories = await MatonOS.fdroidRepositories();
    const results = await Promise.all(repositories.map(async (repository) => {
      try { return await MatonOS.fdroidSearch(repository.url, query); } catch { return []; }
    }));
    const unique = new Map<string, (typeof results)[number][number]>();
    results.flat().forEach((app) => { if (!unique.has(app.id)) unique.set(app.id, app); });
    return Array.from(unique.values()).map((app) => ({ ...app, source: "F-Droid" as const }));
  },
  async details(app) {
    const result = await MatonOS.fdroidDetails(app.repository || "https://f-droid.org/repo", app.id);
    return { ...result, source: "F-Droid" };
  },
  async installed() {
    const repositories = await MatonOS.fdroidRepositories();
    const results = await Promise.all(repositories.map(async (repository) => {
      try { return await MatonOS.fdroidInstalled(repository.url); } catch { return []; }
    }));
    const unique = new Map<string, (typeof results)[number][number]>();
    results.flat().forEach((app) => { if (!unique.has(app.id)) unique.set(app.id, app); });
    return Array.from(unique.values()).map((app) => ({ ...app, source: "F-Droid" as const }));
  },
  async updates() {
    const result = await MatonOS.fdroidUpdates();
    return result.map((app) => ({ ...app, source: "F-Droid" as const }));
  },
  async install(app, operationId) { await MatonOS.fdroidInstall(app.repository || "https://f-droid.org/repo", app.id, operationId); },
  async uninstall(app, operationId) { await MatonOS.fdroidUninstall(app.id, operationId); },
  progress(listener: (event: SourceProgress) => void) {
    return MatonOS.addStoreListener((event) => {
      if (event.source === "F-Droid") listener(event);
    });
  },
};
