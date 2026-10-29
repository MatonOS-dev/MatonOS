import { MatonOS } from "../../modules/matonos-flathub/src/MatonOS";
import { unsupported, type Source, type SourceProgress } from "./Source";

const REPOSITORY = "https://f-droid.org/repo";

export const FdroidSource: Source = {
  name: "F-Droid",
  async search(query) {
    const result = await MatonOS.fdroidSearch(REPOSITORY, query);
    return result.map((app) => ({ ...app, source: "F-Droid" as const }));
  },
  async details(app) {
    const result = await MatonOS.fdroidDetails(REPOSITORY, app.id);
    return { ...result, source: "F-Droid" };
  },
  async installed() {
    const result = await MatonOS.fdroidInstalled(REPOSITORY);
    return result.map((app) => ({ ...app, source: "F-Droid" as const }));
  },
  async updates() { throw unsupported("Updates"); },
  async install(app, operationId) { await MatonOS.fdroidInstall(REPOSITORY, app.id, operationId); },
  async uninstall(app, operationId) { await MatonOS.fdroidUninstall(app.id, operationId); },
  progress(listener: (event: SourceProgress) => void) {
    return MatonOS.addStoreListener((event) => {
      if (event.source === "F-Droid") listener(event);
    });
  },
};
