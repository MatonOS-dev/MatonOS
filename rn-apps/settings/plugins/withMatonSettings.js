const fs = require('fs');
const path = require('path');
const {
  withAndroidManifest,
  withAppBuildGradle,
  withDangerousMod,
  withGradleProperties,
  withSettingsGradle,
} = require('@expo/config-plugins');

module.exports = function withMatonSettings(config) {
  config = withSettingsGradle(config, (mod) => {
    const marker = 'include(":matonos-client")';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nproject(":matonos-client").projectDir = file("../../../buildinfra/client")\n`;
    }
    return mod;
  });
  config = withAndroidManifest(config, (mod) => {
    const manifest = mod.modResults.manifest;
    const app = manifest.application?.[0];
    if (!app) throw new Error('Settings Android manifest has no application');
    for (const activity of app.activity ?? []) {
      activity['intent-filter'] = (activity['intent-filter'] ?? []).filter((filter) => {
        const actions = (filter.action ?? []).map((item) => item.$['android:name']);
        const categories = (filter.category ?? []).map((item) => item.$['android:name']);
        return !(actions.includes('android.intent.action.MAIN') && categories.includes('android.intent.category.LAUNCHER'));
      });
    }
    const put = (list, name, value) => {
      const found = list.find((item) => item.$['android:name'] === name);
      if (found) Object.assign(found.$, value); else list.push({$:{'android:name':name,...value}});
    };
    const activities = app.activity ?? (app.activity = []);
    app.$['android:icon'] = '@mipmap/ic_maton_settings';
    let main = activities.find((item) => item.$['android:name'] === '.MainActivity');
    if (!main) throw new Error('Expo MainActivity missing from generated manifest');
    main.$['android:exported'] = 'true';
    main.$['android:label'] = '@string/settings_tile_title';
    // Reuse the active Router instance so launcher VIEW intents reach Expo Router onNewIntent.
    main.$['android:launchMode'] = 'singleTask';
    main['intent-filter'] = main['intent-filter'] ?? [];
    main['intent-filter'].push({'action':[{$:{'android:name':'com.android.settings.action.IA_SETTINGS'}}]});
    const installAliasActivity = 'org.matonos.settings.InstallAliasActivity';
    if (!activities.some((item) => item.$['android:name'] === installAliasActivity)) {
      activities.push({$:{'android:name':installAliasActivity,'android:exported':'false','android:noHistory':'true','android:excludeFromRecents':'true'}});
    }
    main['meta-data'] = main['meta-data'] ?? [];
    put(main['meta-data'], 'com.android.settings.category', {'android:value':'com.android.settings.category.ia.homepage'});
    put(main['meta-data'], 'com.android.settings.title', {'android:resource':'@string/settings_tile_title'});
    put(main['meta-data'], 'com.android.settings.summary', {'android:resource':'@string/settings_tile_summary'});
    put(main['meta-data'], 'com.android.settings.icon', {'android:resource':'@mipmap/ic_maton_settings'});
    // The account category is first on the Settings homepage; dynamic tiles use this group key.
    put(main['meta-data'], 'com.android.settings.group_key', {'android:value':'top_level_account_category'});
    put(main['meta-data'], 'com.android.settings.order', {'android:value':'-100'});

    app.receiver = app.receiver ?? [];
    app.receiver = app.receiver.filter((item) => item.$['android:name'] !== 'org.matonos.settings.nativebridge.SettingsBootReceiver');
    app.receiver.push({
      $:{'android:name':'org.matonos.settings.nativebridge.SettingsBootReceiver','android:enabled':'true','android:exported':'false'},
      'intent-filter':[{'action':[{$:{'android:name':'android.intent.action.BOOT_COMPLETED'}}]}],
    });
    app['activity-alias'] = app['activity-alias'] ?? [];
    app['activity-alias'] = app['activity-alias'].filter((item) => item.$['android:name'] !== '.InstallAlias');
    app['activity-alias'].push({
      $:{'android:name':'.InstallAlias','android:targetActivity':installAliasActivity,'android:enabled':'false','android:exported':'true','android:label':'@string/install_alias_title','android:icon':'@drawable/ic_maton_install'},
      'intent-filter':[
        {'action':[{$:{'android:name':'android.intent.action.MAIN'}}],'category':[{$:{'android:name':'android.intent.category.LAUNCHER'}}]},
        {'action':[{$:{'android:name':'android.intent.action.VIEW'}}],'category':[{$:{'android:name':'android.intent.category.DEFAULT'}},{$:{'android:name':'android.intent.category.BROWSABLE'}}],'data':[{$:{'android:scheme':'matonos-settings','android:host':'install'}}]},
      ],
    });
    return mod;
  });
  config = withGradleProperties(config, (mod) => {
    const values = new Map(mod.modResults.filter((entry) => entry.type === 'property').map((entry) => [entry.key, entry]));
    for (const [key, value] of [['reactNativeArchitectures','x86_64'],['reactNativeDevServerPort','8084']]) {
      const entry = values.get(key); if (entry) entry.value = value; else mod.modResults.push({type:'property',key,value});
    }
    return mod;
  });
  config = withAppBuildGradle(config, (mod) => {
    const marker = '// MatonOS Settings signing and x86_64-only native payload';
    if (!mod.modResults.contents.includes(marker)) {
      mod.modResults.contents += `\n${marker}\nandroid {\n  defaultConfig { minSdkVersion 32; ndk { abiFilters "x86_64" } }\n  signingConfigs { matonos { storeFile file(System.getenv("MATON_SIGNING_STORE_FILE")); storePassword System.getenv("MATON_SIGNING_STORE_PASSWORD"); keyAlias System.getenv("MATON_SIGNING_KEY_ALIAS"); keyPassword System.getenv("MATON_SIGNING_KEY_PASSWORD") } }\n  buildTypes { debug { signingConfig signingConfigs.matonos }; release { signingConfig signingConfigs.matonos } }\n}\n`;
    }
    return mod;
  });
  config = withDangerousMod(config, ['android', async (mod) => {
    const project = mod.modRequest.projectRoot;
    const android = mod.modRequest.platformProjectRoot;
    const permissionSource = path.join(project, 'privapp-permissions-org.matonos.settings.xml');
    const permissionDestination = path.join(android, 'app/src/main/assets/privapp-permissions-org.matonos.settings.xml');
    fs.mkdirSync(path.dirname(permissionDestination), {recursive:true}); fs.copyFileSync(permissionSource, permissionDestination);
    const values = path.join(android, 'app/src/main/res/values'); fs.mkdirSync(values, {recursive:true});
    fs.copyFileSync(path.join(project, 'modules/matonos-settings/android/src/main/res/values/settings_strings.xml'), path.join(values, 'matonos_settings.xml'));
    const resources = path.join(project, 'modules/matonos-settings/android/src/main/res');
    for (const [folder, files] of [['drawable', ['ic_maton_settings_foreground.xml', 'ic_maton_settings_background.xml', 'ic_maton_install.xml']], ['mipmap-anydpi-v26', ['ic_maton_settings.xml']]]) {
      const destination = path.join(android, 'app/src/main/res', folder); fs.mkdirSync(destination, {recursive:true});
      for (const file of files) fs.copyFileSync(path.join(resources, folder, file), path.join(destination, file));
    }
    return mod;
  }]);
  return config;
};
