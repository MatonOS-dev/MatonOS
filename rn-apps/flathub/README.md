> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Software Center — source plugins

The built-in source is Flathub. Enabled backend apps are loaded through
`SourcePlugins` and wrapped by `PluginSource`, using the contract in
[SOURCE-PLUGINS.md](../../store-sources/contract/SOURCE-PLUGINS.md). System plugins start enabled; the
Sources controls can toggle them and refresh the session source registry.
Only plugins advertising `HAS_REPOSITORIES` get repository management.

F-Droid now lives in the separate Java app
[`store-sources/fdroid`](../../store-sources/fdroid/README.md). It owns trusted
repository preferences, authenticated indices, compatible version selection,
safety mapping and verified APK downloads. The store owns PackageInstaller
sessions and uninstall. `SourceInstallReceiver` handles generic uninstall
results; commit results are relayed directly to each plugin.

Checks: `npx tsc --noEmit -p .`, `npx eslint src`, and the release app build
through `tools/build-apps.sh` (`MATON_APPS_ONLY=matonos-flathub`). The moved
index-client host test is `store-sources/fdroid/test-host.sh`. Fresh-image
runtime checks and known limitations are in the plugin README.
