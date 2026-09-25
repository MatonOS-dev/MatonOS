# MatonOS

Android (AOSP) for generic x86_64 PCs. Stock, unpatched AOSP framework;
MatonOS lives in configuration, our own daemons/HALs/apps and one system
bridge app.

## Layout (mirrors AOSP checkout paths)

| Path | What |
|---|---|
| `device/maton/pc_x86_64/` | Device tree: product config, ODM driver bundle, daemons, sepolicy, system bridge, build/test tools. Start with its `README.md`, `CLAUDE.md` and `NOTES.md`. |
| `apps/launchme/` | MatonOS Shell (launcher), Expo / React Native + Hermes. |
| `apps/rn-common/` | Shared React Native package (theme, MatonOS client wrapper). |
| `forks/<aosp path>/` | Our commits on forked projects, as `git format-patch` series + `BASE` (upstream + base commit). Apply on a local `matonos/v1.2` branch in that project. |

## Using it with an AOSP checkout

1. Sync AOSP, then copy or symlink `device/maton` and `apps/` into the tree.
2. `cp device/maton/pc_x86_64/manifest/maton.xml .repo/local_manifests/`,
   create the `matonos/v1.2` branches from `forks/*/BASE` and `git am` the
   patches, then `repo sync`.
3. Build: `device/maton/pc_x86_64/tools/build.sh` (see the device README).

Build outputs, downloaded APKs, kernel/Mesa prebuilts and signing keys are
not in this repo; the tools rebuild or fetch them.
