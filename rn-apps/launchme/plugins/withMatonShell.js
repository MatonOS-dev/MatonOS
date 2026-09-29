const fs = require('fs');
const path = require('path');
const {withAndroidManifest, withAppBuildGradle, withGradleProperties, withSettingsGradle, withDangerousMod} = require('@expo/config-plugins');

const withMatonShell = (config) => {
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
    // Expo's generated entry point is retained for dev-client launches, while
    // HomeActivity in the local module owns the HOME intent and all product UX.
    for (const activity of app.activity ?? []) {
      const filters = activity['intent-filter'] ?? [];
      activity['intent-filter'] = filters.filter((filter) => {
        const actions = (filter.action ?? []).map((item) => item.$['android:name']);
        const categories = (filter.category ?? []).map((item) => item.$['android:name']);
        return !(actions.includes('android.intent.action.MAIN') && categories.includes('android.intent.category.LAUNCHER'));
      });
    }
    return mod;
  });

  config = withGradleProperties(config, (mod) => {
    const entry = mod.modResults.find((item) => item.type === 'property' && item.key === 'reactNativeArchitectures');
    if (entry) entry.value = 'x86_64';
    else mod.modResults.push({type: 'property', key: 'reactNativeArchitectures', value: 'x86_64'});
    return mod;
  });

  config = withAppBuildGradle(config, (mod) => {
    const marker = '// MatonOS signing and x86_64-only native payload';
    if (!mod.modResults.contents.includes(marker)) {
      const block = `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters "x86_64" } }\n  signingConfigs {\n    matonos {\n      storeFile file(System.getenv("MATON_SIGNING_STORE_FILE"))\n      storePassword System.getenv("MATON_SIGNING_STORE_PASSWORD")\n      keyAlias System.getenv("MATON_SIGNING_KEY_ALIAS")\n      keyPassword System.getenv("MATON_SIGNING_KEY_PASSWORD")\n    }\n  }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\n`;
      mod.modResults.contents += block;
    }
    return mod;
  });

  config = withDangerousMod(config, ['android', async (mod) => {
    const source = path.join(mod.modRequest.projectRoot, 'privapp-permissions-org.matonos.shell.xml');
    const destination = path.join(mod.modRequest.platformProjectRoot, 'app/src/main/assets/privapp-permissions-org.matonos.shell.xml');
    fs.mkdirSync(path.dirname(destination), {recursive: true});
    fs.copyFileSync(source, destination);
    return mod;
  }]);
  return config;
};

module.exports = withMatonShell;
