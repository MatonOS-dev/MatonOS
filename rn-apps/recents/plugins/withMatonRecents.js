const fs = require('fs');
const path = require('path');
const {withAndroidManifest, withAppBuildGradle, withGradleProperties, withSettingsGradle, withDangerousMod} = require('@expo/config-plugins');

const withMatonRecents = (config) => {
  config = withSettingsGradle(config, (mod) => {
    const marker = 'include(":matonos-client")';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nproject(":matonos-client").projectDir = file("../../../buildinfra/client")\n`;
    }
    return mod;
  });
  config = withAndroidManifest(config, (mod) => {
    const app = mod.modResults.manifest.application?.[0];
    if (!app) return mod;
    for (const activity of app.activity ?? []) {
      activity['intent-filter'] = (activity['intent-filter'] ?? []).filter((filter) => {
        const actions = (filter.action ?? []).map((item) => item.$['android:name']);
        const categories = (filter.category ?? []).map((item) => item.$['android:name']);
        return !(actions.includes('android.intent.action.MAIN') && categories.includes('android.intent.category.LAUNCHER'));
      });
    }
    return mod;
  });
  config = withGradleProperties(config, (mod) => {
    const values = new Map(mod.modResults.filter((entry) => entry.type === 'property').map((entry) => [entry.key, entry]));
    for (const [key, value] of [['reactNativeArchitectures', 'x86_64'], ['reactNativeDevServerPort', '8083']]) {
      const entry = values.get(key);
      if (entry) entry.value = value;
      else mod.modResults.push({type: 'property', key, value});
    }
    return mod;
  });
  config = withAppBuildGradle(config, (mod) => {
    const marker = '// MatonOS Recents signing and release alias';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters "x86_64" } }\n  signingConfigs { matonos { storeFile file(System.getenv("MATON_SIGNING_STORE_FILE")); storePassword System.getenv("MATON_SIGNING_STORE_PASSWORD"); keyAlias System.getenv("MATON_SIGNING_KEY_ALIAS"); keyPassword System.getenv("MATON_SIGNING_KEY_PASSWORD") } }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\ntasks.register("expo") { dependsOn("assembleRelease") }\n`;
    }
    return mod;
  });
  config = withDangerousMod(config, ['android', async (mod) => {
    const source = path.join(mod.modRequest.projectRoot, 'privapp-permissions-org.matonos.recents.xml');
    const destination = path.join(mod.modRequest.platformProjectRoot, 'app/src/main/assets/privapp-permissions-org.matonos.recents.xml');
    fs.mkdirSync(path.dirname(destination), {recursive: true});
    fs.copyFileSync(source, destination);
    return mod;
  }]);
  return config;
};

module.exports = withMatonRecents;
