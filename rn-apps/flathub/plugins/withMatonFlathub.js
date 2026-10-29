const fs = require("fs");
const path = require("path");
const { withAndroidManifest, withAppBuildGradle, withDangerousMod, withGradleProperties, withSettingsGradle } = require("@expo/config-plugins");

module.exports = function withMatonFlathub(config) {
  config = withSettingsGradle(config, (mod) => {
    const marker = 'include(":matonos-client")';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nproject(":matonos-client").projectDir = file("../../../buildinfra/client")\n`;
    }
    return mod;
  });
  config = withGradleProperties(config, (mod) => {
    const entry = mod.modResults.find((item) => item.type === "property" && item.key === "reactNativeArchitectures");
    if (entry) entry.value = "x86_64";
    else mod.modResults.push({ type: "property", key: "reactNativeArchitectures", value: "x86_64" });
    return mod;
  });
  // Launcher icon: adaptive vector icon from plugins/icon (no Expo PNG icon).
  config = withAndroidManifest(config, (mod) => {
    const app = mod.modResults.manifest.application?.[0];
    if (!app) throw new Error("Software Center Android manifest has no application");
    app.$["android:icon"] = "@mipmap/ic_maton_store";
    app.$["android:roundIcon"] = "@mipmap/ic_maton_store";
    const activity = mod.modResults.manifest.application[0].activity?.find((item) => item.$?.["android:name"] === ".MainActivity");
    if (!activity) throw new Error("Software Center MainActivity is missing");
    const filters = activity["intent-filter"] || (activity["intent-filter"] = []);
    if (!filters.some((filter) => filter.data?.some((item) => item.$?.["android:scheme"] === "fdroidrepos"))) {
      filters.push({ action: [{ $: { "android:name": "android.intent.action.VIEW" } }], category: [
        { $: { "android:name": "android.intent.category.DEFAULT" } },
        { $: { "android:name": "android.intent.category.BROWSABLE" } },
      ], data: [{ $: { "android:scheme": "fdroidrepos" } }] });
    }
    return mod;
  });
  config = withDangerousMod(config, ["android", async (mod) => {
    const source = path.join(mod.modRequest.projectRoot, "plugins/icon");
    for (const folder of fs.readdirSync(source)) {
      const destination = path.join(mod.modRequest.platformProjectRoot, "app/src/main/res", folder);
      fs.mkdirSync(destination, { recursive: true });
      for (const file of fs.readdirSync(path.join(source, folder))) fs.copyFileSync(path.join(source, folder, file), path.join(destination, file));
    }
    return mod;
  }]);
  return withAppBuildGradle(config, (mod) => {
    const marker = "// MatonOS Flathub signing and x86_64-only native payload";
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters \"x86_64\" } }\n  signingConfigs { matonos { storeFile file(System.getenv(\"MATON_SIGNING_STORE_FILE\")); storePassword System.getenv(\"MATON_SIGNING_STORE_PASSWORD\"); keyAlias System.getenv(\"MATON_SIGNING_KEY_ALIAS\"); keyPassword System.getenv(\"MATON_SIGNING_KEY_PASSWORD\") } }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\n`;
    }
    return mod;
  });
};
