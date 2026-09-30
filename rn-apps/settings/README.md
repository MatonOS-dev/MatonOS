# MatonOS Settings

## Structure

The root Expo Router layout renders the persistent MatonOS shell. Its `Host`, theme, live-image/system-info context, navigation rail, back button, and `<Slot />` stay mounted while the selected route changes. `app/index.tsx` is the only Sleep route and the default page; each section has its own route and component under `src/sections/`. Sleep, hardware, and installer state is owned by those section components and refreshed when their route gains focus.

Material Symbols are requested through a process-wide promise/source cache. The Settings tile opens the default Sleep route. On a live image the Install MatonOS launcher alias targets a small native forwarding activity, which starts MainActivity with `matonos-settings://install`; Expo Router resolves that URI directly to `/install`. The install route displays an unavailable message when opened on an installed image, while the alias itself stays disabled there.

The back button returns through the in-app route history and finishes the Settings activity when there is no earlier route. The injected Settings tile is grouped in Android Settings' existing `top_level_account_category`, whose `-140` order places it above the connectivity category and Network & internet; its tile order is `-100` inside that group. Settings sections and reusable Compose UI pieces are separate components. The short fixed navigation rail is written as explicit `NavItem` instances; generated drive and operation lists render through `FlatList` components.

## Verification

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
