import React, {useEffect, useMemo, useState} from 'react';
import {FlatList, Image, Pressable, StyleSheet, Text, useWindowDimensions} from 'react-native';
import {Appbar, IconButton, Searchbar, Surface, useTheme} from 'react-native-paper';
import {MatonShell, type LauncherApp} from '../MatonShellNative';
import {ComposeUiPreview} from '../components/ComposeUiPreview';

export function AppDrawerScreen(): React.JSX.Element {
  const theme = useTheme();
  const window = useWindowDimensions();
  const [apps, setApps] = useState<LauncherApp[]>([]);
  const [query, setQuery] = useState('');
  const columns = Math.max(4, Math.floor(window.width / 168));
  const cellWidth = Math.floor((window.width - 32 - columns * 16) / columns);
  useEffect(() => {
    let live = true;
    void MatonShell.getLauncherApps()
      .then((items) => {
        if (live) setApps(items);
      })
      .catch(() => {});
    const changes = MatonShell.onPackagesChanged(() => {
      void MatonShell.getLauncherApps()
        .then((items) => {
          if (live) setApps(items);
        })
        .catch(() => {});
    });
    return () => {
      live = false;
      changes.remove();
    };
  }, []);
  const filtered = useMemo(
    () =>
      apps.filter((app) =>
        `${app.label} ${app.packageName}`.toLocaleLowerCase().includes(query.toLocaleLowerCase()),
      ),
    [apps, query],
  );
  return (
    <Surface style={[styles.panel, {backgroundColor: theme.colors.surface}]}>
      <Appbar.Header>
        <Appbar.Content title="Apps" />
        <IconButton icon="home" accessibilityLabel="Home" onPress={MatonShell.goHome} />
      </Appbar.Header>
      <Searchbar
        placeholder="Search apps"
        value={query}
        onChangeText={setQuery}
        style={styles.search}
      />
      {__DEV__ && <ComposeUiPreview />}
      <FlatList
        data={filtered}
        key={columns}
        numColumns={columns}
        keyExtractor={(app) => app.component}
        contentContainerStyle={styles.grid}
        keyboardShouldPersistTaps="handled"
        renderItem={({item}) => (
          <LauncherAppTile
            app={item}
            width={cellWidth}
            foreground={theme.colors.onSurface}
            highlight={theme.colors.secondaryContainer}
            outline={theme.colors.primary}
          />
        )}
        ListEmptyComponent={
          <Text style={[styles.empty, {color: theme.colors.onSurfaceVariant}]}>
            No matching apps
          </Text>
        }
      />
    </Surface>
  );
}

function LauncherAppTile({
  app,
  width,
  foreground,
  highlight,
  outline,
}: {
  app: LauncherApp;
  width: number;
  foreground: string;
  highlight: string;
  outline: string;
}): React.JSX.Element {
  const [highlighted, setHighlighted] = useState(false);
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={`Open ${app.label}`}
      focusable
      onPress={() => void MatonShell.launchApp(app.component)}
      onLongPress={() => void MatonShell.togglePinnedApp(app.packageName)}
      onHoverIn={() => setHighlighted(true)}
      onHoverOut={() => setHighlighted(false)}
      onFocus={() => setHighlighted(true)}
      onBlur={() => setHighlighted(false)}
      style={[styles.cell, {width}, highlighted && {backgroundColor: highlight, borderColor: outline}]}
    >
      {!!app.iconUri && <Image source={{uri: app.iconUri}} style={styles.icon} />}
      <Text numberOfLines={2} style={[styles.label, {color: foreground}]}>
        {app.label}
      </Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  panel: {flex: 1, backgroundColor: 'rgba(24,28,34,0.96)'},
  search: {marginHorizontal: 22, marginVertical: 14},
  grid: {padding: 16},
  cell: {
    minHeight: 130,
    margin: 8,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: 16,
    gap: 10,
    borderWidth: 2,
    borderColor: 'transparent',
  },
  icon: {width: 56, height: 56},
  label: {color: 'white', textAlign: 'center', fontSize: 14, maxWidth: 132},
  empty: {color: 'white', textAlign: 'center', padding: 36},
});
