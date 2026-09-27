const {withAndroidManifest, withAppBuildGradle, withGradleProperties} = require('@expo/config-plugins');

module.exports = function withInstallerDormant(config) {
  config = withAndroidManifest(config, config => {
    const application = config.modResults.manifest.application?.[0];
    if (!application) throw new Error('Installer Android manifest has no application element');
    application.$['android:enabled'] = 'false';
    return config;
  });
  config = withGradleProperties(config, mod => {
    const values = new Map(mod.modResults.filter(entry => entry.type === 'property').map(entry => [entry.key, entry]));
    for (const [key, value] of [['reactNativeArchitectures', 'x86_64'], ['reactNativeDevServerPort', '8084']]) {
      const entry = values.get(key);
      if (entry) entry.value = value;
      else mod.modResults.push({type: 'property', key, value});
    }
    return mod;
  });
  config = withAppBuildGradle(config, mod => {
    const marker = '// MatonOS Installer signing and release alias';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters "x86_64" } }\n  signingConfigs { matonos { storeFile file(System.getenv("MATON_SIGNING_STORE_FILE")); storePassword System.getenv("MATON_SIGNING_STORE_PASSWORD"); keyAlias System.getenv("MATON_SIGNING_KEY_ALIAS"); keyPassword System.getenv("MATON_SIGNING_KEY_PASSWORD") } }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\ntasks.register("expo") { dependsOn("assembleRelease") }\n`;
    }
    return mod;
  });
  return config;
};
