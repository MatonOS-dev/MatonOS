# Window caption and taskbar tray

## What the user sees

Each desktop app window has a caption with the app icon and title. It has close, maximize/restore and minimize buttons. The title can sit in the center or at the start, and the buttons can move between the left and right sides. The caption app owns this appearance and redraws it when its preferences change ([`TaskCaption.java`](../caption/app/src/main/java/org/matonos/caption/TaskCaption.java#L74)).

Maximize enters desktop immersive mode. The window fills the display and the status bar and caption hide; moving the pointer to the top edge reveals them, and restore is available from the revealed caption. A fresh-image VM run recorded this behavior ([`HANDOVER-2026-11-22-morning.md`](../HANDOVER-2026-11-22-morning.md#L181)).

The Launcher3 taskbar has a tray pill at its left edge. Tray icons and menus are rendered by the selected tray host inside the pill. The pill is empty when no host is connected.

## How it works

MatonSystemUI asks the selected caption provider to render a surface for each task. It passes task state, including the title, icon, focus, maximize state and whether the app draws its own decorations ([`CaptionProviderController.java`](../apps/MatonSystemUI/src/org/matonos/systemui/CaptionProviderController.java#L257)). The default provider draws the app identity and three buttons. Its button callbacks go back to Shell; maximize calls Shell's immersive-or-restore action ([`TaskCaption.java`](../caption/app/src/main/java/org/matonos/caption/TaskCaption.java#L97), [`CaptionProviderController.java`](../apps/MatonSystemUI/src/org/matonos/systemui/CaptionProviderController.java#L409)).

The provider also reports which parts of its surface should receive input. Buttons and the app identity are interactive; the rest remains Shell's caption drag area. For client-decorated apps, or when buttons are disallowed, the provider draws no caption and reports no interactive regions ([`TaskCaption.java`](../caption/app/src/main/java/org/matonos/caption/TaskCaption.java#L74), [`TaskCaption.java`](../caption/app/src/main/java/org/matonos/caption/TaskCaption.java#L188)).

Approved patch `0004-WM-Shell-add-optional-desktop-caption-provider-hook.patch` adds an optional `DesktopCaptionFactory` hook to WM Shell. It lets a SystemUI variant attach provider surfaces per task; with no factory, stock behavior remains. It adds a touchable-region API so provider buttons receive input while the rest of the header remains a drag strip. It also wires maximize to desktop immersive mode ([patch](../patches/frameworks/base/0004-WM-Shell-add-optional-desktop-caption-provider-hook.patch#L4)). This is the approved exception to the zero-patch rule.

Linux apps can publish StatusNotifierItem tray entries over D-Bus. The subsystem's D-Bus glue acts as the StatusNotifierWatcher and hands the entries to its tray host; the design and exception for apps that hide to the tray are recorded in [`NOTES.md`](../NOTES.md#L1026). The Launcher3 patch defines the host/listener contract, including item-change notifications and activation/menu requests ([Launcher3 patch](../patches/packages/apps/Launcher3/0001-linux-status-tray.patch#L87)). Launcher3 binds the selected host and embeds its SurfaceControlViewHost pill at the taskbar's left edge. The host draws the icons and handles pill input and menus ([Launcher3 patch](../patches/packages/apps/Launcher3/0001-linux-status-tray.patch#L638)).

MatonOS Settings' System components page discovers caption and tray services that hold the required binding permission, then stores the chosen component in secure settings ([`SystemComponents.java`](../rn-apps/settings/modules/matonos-settings/android/src/main/java/org/matonos/settings/SystemComponents.java#L17)). The page presents separate provider choices for captions and tray ([`SystemComponentsSection.tsx`](../rn-apps/settings/src/sections/SystemComponentsSection.tsx#L123)). Choosing the default clears the override; selecting a third-party provider asks for confirmation first ([same file](../rn-apps/settings/src/sections/SystemComponentsSection.tsx#L97)).

## Files

- [`patches/frameworks/base/0004-WM-Shell-add-optional-desktop-caption-provider-hook.patch`](../patches/frameworks/base/0004-WM-Shell-add-optional-desktop-caption-provider-hook.patch) — Shell caption-provider hook, touch regions and immersive maximize behavior.
- [`apps/MatonSystemUI/src/org/matonos/systemui/CaptionProviderController.java`](../apps/MatonSystemUI/src/org/matonos/systemui/CaptionProviderController.java) — binds providers and bridges task actions to Shell.
- [`caption/`](../caption/) — default caption provider app, rendering and preferences.
- [`patches/packages/apps/Launcher3/0001-linux-status-tray.patch`](../patches/packages/apps/Launcher3/0001-linux-status-tray.patch) — Launcher3 pill slot and host protocol.
- [`rn-apps/settings/src/sections/SystemComponentsSection.tsx`](../rn-apps/settings/src/sections/SystemComponentsSection.tsx) and [`SystemComponents.java`](../rn-apps/settings/modules/matonos-settings/android/src/main/java/org/matonos/settings/SystemComponents.java) — provider selection UI and service discovery.
- Linux StatusNotifierItem watcher and tray-host implementation — subsystem repository, outside this device-tree worktree. The current tree records the integration contract and VM milestone in [`NOTES.md`](../NOTES.md#L1026) and [`HANDOVER-2026-11-22-morning.md`](../HANDOVER-2026-11-22-morning.md#L174).

## Known limits

- The StatusNotifierItem watcher and tray-host source are not in this repository, so this document can cite their design and integration milestone but not their implementation lines.
- The recorded VM milestone confirms the caption and provider selection page. It does not record a live tray icon/menu interaction test.
