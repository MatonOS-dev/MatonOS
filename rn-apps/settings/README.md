# MatonOS Settings

## Structure

The root Expo Router layout renders the persistent MatonOS shell. Its `Host`, theme, live-image/system-info context, navigation rail, back button, and `<Slot />` stay mounted while the selected route changes. `app/index.tsx` is the only Sleep route and the default page; each section has its own route and component under `src/sections/`. Sleep, hardware, and installer state is owned by those section components and refreshed when their route gains focus.

Material Symbols are requested through a process-wide promise/source cache. The Settings tile opens the default Sleep route. On a live image the Install MatonOS launcher alias targets a small native forwarding activity, which starts MainActivity with `matonos-settings://install`; Expo Router resolves that URI directly to `/install`. The install route displays an unavailable message when opened on an installed image, while the alias itself stays disabled there.

The back button returns through the in-app route history and finishes the Settings activity when there is no earlier route. The injected Settings tile is grouped in Android Settings' existing `top_level_account_category`, whose `-140` order places it above the connectivity category and Network & internet; its tile order is `-100` inside that group. Settings sections and reusable Compose UI pieces are separate components. The short fixed navigation rail is written as explicit `NavItem` instances; generated drive and operation lists render through `FlatList` components.

## Verification

The **Media volume** Quick Settings tile is a Java `TileService` in the local
Android module. Its manifest and resources merge into the Settings APK during
Expo CNG. The SystemUI RRO puts its custom component third in both default QS
lists. It opens a transparent, non-exported activity through
`startActivityAndCollapse(PendingIntent)`, then requests SystemUI's media slider
with `STREAM_MUSIC`, `ADJUST_SAME`, and `FLAG_SHOW_UI`. Opening it does not change
volume or mute, does not need a running media session, and uses only public
Android APIs (no bridge capability or privileged permission). Locked tiles wait
for unlock. Android owns the slider, mute control and expanded volume panel.

For an app-only build from a detached device-tree worktree, set
`MATON_AOSP_ROOT=/mnt/data/aosp MATON_APPS_ONLY=matonos-settings` when running
`tools/build-apps.sh`. The override supplies shared checkout SDK stubs; build
outputs still go into this worktree.

After the rebuilt Settings APK and SystemUI overlay are integrated into an
image, verify Media volume appears near the front of QS on fresh user data. Click
it while idle and during playback, drag the media slider with the mouse, and
check mute/unmute and repeated clicks. Verify a click alone preserves the level,
the shade collapses, and no blank window remains in Recents. On existing user
data, add Media volume through QS Edit: the default overlay deliberately does
not overwrite a user's saved `sysui_qs_tiles` list. Repeat after reboot and on
the lock screen (unlock must precede opening). PC volume keys already use the
stock mappings in `input/keylayout/Vendor_4d54_Product_0001.kl`.

- `npm run typecheck`, `npm run lint`, and `node --check plugins/withMatonSettings.js` — passed.
- Static scan — no `getInitialSection` or `router.replace`; no `.map()` in JSX (drive and operation lists render through `FlatList` components; other `.map()` calls are state updates before the return).
- APK release build, Expo prebuild, and release-linkage check passed. `aapt2 dump xmltree` confirmed the staged APK declares `com.android.settings.group_key=top_level_account_category`, order `-100`, and the `matonos-settings://install` alias filter.
- Fresh boot on the final coordinator image dated 2026-09-28 15:05:08 — Android Settings showed MatonOS above Network & internet. Opening the tile started at `/` (Sleep); Hardware, Developer, and About rendered with the rail present. In-app Back returned to the preceding route, and top-level Back finished MatonOS Settings and returned to Android Settings. `.InstallAlias` opened `/install` both from a cold launch and while MainActivity was already running, confirming that its VIEW intent reaches Expo Router without remounting the app. Five idle Sleep captures 0.5 seconds apart were byte-identical (SHA-256 `6c853ac9fd66a99dc94980bfab2e1b43bbe84771502b99bc95160d168000c134`). Captures and boot logs are under `/mnt/data/aosp/out/pc-logs/settings/`.
- The Install route renders, but its installer bridge request reports `BRIDGE_APP_NOT_TRUSTED:install` on the live image. The same error appeared in the earlier image before the router refactor, so drive/install service authorization still needs integration follow-up.
- Two earlier shared full-build attempts failed at patch application because `frameworks/base/0003-pinned-gms-update.patch` had stale framework changes applied. The coordinator swapped the framework state and the next full build completed successfully at 15:05:10. SELinux label verification reports all 9 init-started vendor programs have exec labels.

## Real-PC checks

1. Boot a fresh live MatonOS image on the Ryzen 5800X/RX 6600/Intel 7265 system or HP ProDesk 600 G1. Open Settings → MatonOS Settings and verify that Sleep is the initial page.
2. Visit Sleep, Hardware, Developer, and About. Confirm each route title/content, that rail icons stay visible while navigating, and that Back returns to the previous section and then to Android Settings.
3. From the live image's launcher, open Install MatonOS. Confirm that it opens the Install page directly, without first showing Sleep. Check drive detection, refresh, plan review, and erase confirmation; do not confirm an erase on a disk containing data.
4. Boot an installed image and verify that the Install launcher alias is absent. Opening `/install` directly should show the live-image-only message.
5. Check sleep controls on the Intel HDA / Surface Pro 3 system and hardware reporting with Wi-Fi/BT present and absent. Confirm unavailable hardware remains a clean empty/unknown state.
