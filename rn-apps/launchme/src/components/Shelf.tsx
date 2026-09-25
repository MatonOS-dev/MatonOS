import React, {useEffect, useState} from 'react';
import {Image, Pressable, StyleSheet, Text, View} from 'react-native';
import {IconButton, useTheme} from 'react-native-paper';
import {MatonShell, type LauncherApp} from '../MatonShellNative';

export function Shelf(): React.JSX.Element {
  const theme = useTheme();
  const [expanded, setExpanded] = useState(true);
  const [home, setHome] = useState(true);
  const [apps, setApps] = useState<LauncherApp[]>([]);
  const [pinned, setPinned] = useState<string[]>([]);
  const [runningPackages, setRunningPackages] = useState<string[]>([]);
  useEffect(() => {
    let live = true;
    const refresh = (): void => {
      void Promise.all([
        MatonShell.getShelfState(),
        MatonShell.getLauncherApps(),
        MatonShell.getPinnedApps(),
        MatonShell.getRecentTasks(),
      ])
        .then(([state, installed, pins, tasks]) => {
          if (live) {
            setExpanded(state.expanded);
            setHome(state.homeVisible);
            setApps(installed);
            setPinned(pins);
            setRunningPackages([...new Set(tasks.map((task) => task.packageName))]);
          }
        })
        .catch(() => {});
    };
    refresh();
    const stateListener = MatonShell.onShelfState(refresh);
    const packageListener = MatonShell.onPackagesChanged(refresh);
    return () => {
      live = false;
      stateListener.remove();
      packageListener.remove();
    };
  }, []);
  const shelfPackages = new Set([...pinned, ...runningPackages]);
  const shelfApps = apps.filter((app) => shelfPackages.has(app.packageName)).slice(0, 10);
  const togglePage = (page: 'drawer' | 'recents'): void => MatonShell.openPanel(page);
  const expand = (): void => {
    setExpanded(true);
    MatonShell.setShelfExpanded(true);
  };
  return (
    <Pressable
      style={[
        styles.shelf,
        {backgroundColor: theme.colors.surfaceVariant},
        !expanded && styles.compact,
      ]}
      onPress={() => {
        if (!home && !expanded) expand();
      }}
      onHoverIn={() => {
        if (!home && !expanded) expand();
      }}
      onHoverOut={() => {
        if (!home && expanded) {
          setExpanded(false);
          MatonShell.setShelfExpanded(false);
        }
      }}
    >
      {expanded && (
        <IconButton
          icon="arrow-left"
          size={23}
          accessibilityLabel="Back"
          iconColor={theme.colors.onSurface}
          onPress={() => void MatonShell.injectBackKey()}
        />
      )}
      {expanded && (
        <Pressable
          onPress={() => togglePage('drawer')}
          onLongPress={MatonShell.goHome}
          accessibilityRole="button"
          accessibilityLabel="Apps, hold for Home"
          style={styles.appsButton}
        >
          <Text style={[styles.text, {color: theme.colors.onSurface}]}>Apps</Text>
        </Pressable>
      )}
      {expanded &&
        shelfApps.map((app) => (
          <Pressable
            key={app.packageName}
            onPress={() =>
              void (runningPackages.includes(app.packageName)
                ? MatonShell.getRecentTasks()
                    .then((tasks) => tasks.find((task) => task.packageName === app.packageName))
                    .then((task) =>
                      task
                        ? MatonShell.moveTaskToFront(task.taskId)
                        : MatonShell.launchApp(app.component),
                    )
                : MatonShell.launchApp(app.component))
            }
            onLongPress={() => {
              void MatonShell.togglePinnedApp(app.packageName).then((isPinned) => {
                setPinned((current) =>
                  isPinned
                    ? [...current, app.packageName]
                    : current.filter((pkg) => pkg !== app.packageName),
                );
              });
            }}
            style={styles.appButton}
            accessibilityRole="button"
            accessibilityLabel={app.label}
          >
            {!!app.iconUri && <Image source={{uri: app.iconUri}} style={styles.icon} />}
          </Pressable>
        ))}
      <View style={styles.spacer} />
      {expanded && (
        <IconButton
          icon="view-grid"
          size={23}
          accessibilityLabel="Recent apps"
          iconColor={theme.colors.onSurface}
          onPress={() => togglePage('recents')}
        />
      )}
      {!home && !expanded && <Text style={[styles.text, {color: theme.colors.onSurface}]}>⌃</Text>}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  shelf: {
    height: 56,
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: 'rgba(20,24,30,0.92)',
    paddingHorizontal: 6,
  },
  compact: {height: 28},
  appsButton: {minWidth: 62, height: 44, alignItems: 'center', justifyContent: 'center'},
  text: {color: 'white', fontWeight: '600'},
  appButton: {width: 44, height: 46, alignItems: 'center', justifyContent: 'center'},
  icon: {width: 26, height: 26},
  spacer: {flex: 1},
  expand: {width: 48, alignItems: 'center'},
});
