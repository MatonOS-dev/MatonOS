import { MatonOS } from "../../modules/matonos-flathub/src/MatonOS";
import { unsupported, type Source } from "./Source";

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
  async installed() { return []; },
  async updates() { throw unsupported("Updates"); },
  async install() { throw unsupported("Install"); },
  async uninstall() { throw unsupported("Uninstall"); },
  progress() { return { remove() { /* Read-only source has no operations. */ } }; },
};
