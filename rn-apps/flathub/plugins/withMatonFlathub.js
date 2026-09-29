const { withAppBuildGradle, withGradleProperties, withSettingsGradle } = require("@expo/config-plugins");

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
  return withAppBuildGradle(config, (mod) => {
    const marker = "// MatonOS Flathub signing and x86_64-only native payload";
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters \"x86_64\" } }\n  signingConfigs { matonos { storeFile file(System.getenv(\"MATON_SIGNING_STORE_FILE\")); storePassword System.getenv(\"MATON_SIGNING_STORE_PASSWORD\"); keyAlias System.getenv(\"MATON_SIGNING_KEY_ALIAS\"); keyPassword System.getenv(\"MATON_SIGNING_KEY_PASSWORD\") } }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\n`;
    }
    return mod;
  });
};
