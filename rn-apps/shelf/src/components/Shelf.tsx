import React, {useEffect, useRef, useState} from 'react';
import {Image, Pressable, StyleSheet, Text, View} from 'react-native';
import {IconButton, Menu, useTheme} from 'react-native-paper';
import {MatonOS} from '@matonos/rn-common';
import {MatonShelf, type LauncherApp} from '../ShelfNative';

export function Shelf(): React.JSX.Element {
  const theme = useTheme();
  const [expanded, setExpanded] = useState(true);
  const [home, setHome] = useState(true);
  const [apps, setApps] = useState<LauncherApp[]>([]);
  const [pinned, setPinned] = useState<string[]>([]);
  const [runningPackages, setRunningPackages] = useState<string[]>([]);
  const [focusedPackage, setFocusedPackage] = useState('');
  const [threeButtonMode, setThreeButtonMode] = useState(false);
  const [settingsVisible, setSettingsVisible] = useState(false);
  const longBackTriggered = useRef(false);
  useEffect(() => {
    let live = true;
    const refresh = (): void => {
      void Promise.all([
        MatonShelf.getShelfState(),
        MatonShelf.getLauncherApps(),
        MatonShelf.getPinnedApps(),
        MatonOS.getRecentTasks(),
      ])
        .then(([state, installed, pins, tasks]) => {
          if (live) {
            setExpanded(state.expanded);
            setHome(state.homeVisible);
            setThreeButtonMode(state.threeButtonMode);
            setApps(installed);
            setPinned(pins);
            setRunningPackages([...new Set(tasks.map((task) => task.packageName))]);
            setFocusedPackage(tasks[0]?.packageName ?? '');
          }
        })
        .catch(() => {});
    };
    refresh();
    const stateListener = MatonShelf.onShelfState(refresh);
    const packageListener = MatonShelf.onPackagesChanged(refresh);
    const timer = setInterval(refresh, 3000);
    return () => {
      live = false;
      clearInterval(timer);
      stateListener.remove();
      packageListener.remove();
    };
  }, []);
  const shelfPackages = new Set([...pinned, ...runningPackages]);
  const shelfApps = apps
    .filter((app) => shelfPackages.has(app.packageName))
    .slice(0, threeButtonMode ? 5 : 10);
  const openDrawer = (): void => MatonShelf.openPanel('drawer');
  const openRecents = (): void => {
    void MatonOS.navigate('recents').then((ok) => {
      if (!ok) MatonShelf.openPanel('recents');
    });
  };
  const goHome = (): void => {
    if (home) {
      openDrawer();
      return;
    }
    void MatonOS.navigate('home').then((ok) => {
      if (!ok) MatonShelf.goHome();
    });
  };
  const goBack = (): void => {
    void MatonOS.navigate('back');
  };
  const expand = (): void => {
    setExpanded(true);
    MatonShelf.setShelfExpanded(true);
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
          MatonShelf.setShelfExpanded(false);
        }
      }}
    >
      {expanded && (
        <IconButton
          icon="arrow-left"
          size={23}
          accessibilityLabel="Back"
          iconColor={theme.colors.onSurface}
          onPress={() => {
            if (longBackTriggered.current) {
              longBackTriggered.current = false;
              return;
            }
            goBack();
          }}
          onLongPress={() => {
            longBackTriggered.current = true;
            void MatonOS.navigate('back', true);
          }}
        />
      )}
      {expanded && !threeButtonMode && (
        <Pressable
          onPress={openDrawer}
          onLongPress={goHome}
          accessibilityRole="button"
          accessibilityLabel="Apps, hold for Home"
          style={styles.appsButton}
        >
          <Text style={[styles.text, {color: theme.colors.onSurface}]}>Apps</Text>
        </Pressable>
      )}
      {expanded && (
        <Menu
          visible={settingsVisible}
          onDismiss={() => setSettingsVisible(false)}
          anchor={
            <IconButton
              icon="cog"
              size={21}
              accessibilityLabel="Shelf settings"
              iconColor={theme.colors.onSurface}
              onPress={() => setSettingsVisible(true)}
            />
          }
        >
          <Menu.Item
            title={threeButtonMode ? 'Use default shelf layout' : 'Use 3-button navigation'}
            onPress={() => {
              const next = !threeButtonMode;
              setThreeButtonMode(next);
              MatonShelf.setThreeButtonMode(next);
              setSettingsVisible(false);
            }}
          />
        </Menu>
      )}
      {expanded &&
        shelfApps.map((app) => (
          <Pressable
            key={app.packageName}
            onPress={() =>
              void (runningPackages.includes(app.packageName)
                ? MatonOS.getRecentTasks()
                    .then((tasks) => tasks.find((task) => task.packageName === app.packageName))
                    .then((task) =>
                      task
                        ? MatonOS.moveTaskToFront(task.taskId)
                        : MatonShelf.launchApp(app.component),
                    )
                : MatonShelf.launchApp(app.component))
            }
            onLongPress={() => {
              void MatonShelf.togglePinnedApp(app.packageName).then((isPinned) => {
                setPinned((current) =>
                  isPinned
                    ? [...current, app.packageName]
                    : current.filter((pkg) => pkg !== app.packageName),
                );
              });
            }}
            style={[
              styles.appButton,
              focusedPackage === app.packageName && {
                borderColor: theme.colors.primary,
                borderWidth: 2,
              },
            ]}
            accessibilityRole="button"
            accessibilityLabel={app.label}
          >
            {!!app.iconUri && <Image source={{uri: app.iconUri}} style={styles.icon} />}
          </Pressable>
        ))}
      <View style={styles.spacer} />
      {expanded && threeButtonMode && (
        <IconButton
          icon="home"
          size={23}
          accessibilityLabel="Home"
          iconColor={theme.colors.onSurface}
          onPress={goHome}
          style={styles.homeButton}
        />
      )}
      {expanded && threeButtonMode && <View style={styles.spacer} />}
      {expanded && (
        <IconButton
          icon="view-grid"
          size={23}
          accessibilityLabel="Recent apps"
          iconColor={theme.colors.onSurface}
          onPress={openRecents}
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
  homeButton: {position: 'absolute', left: '50%', transform: [{translateX: -24}]},
  icon: {width: 26, height: 26},
  spacer: {flex: 1},
  expand: {width: 48, alignItems: 'center'},
});
