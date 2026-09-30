import React, {useEffect, useState} from 'react';
import {Image, Pressable, StyleSheet, Text, View} from 'react-native';
import {useTheme} from 'react-native-paper';
import {MatonShell, type LauncherApp} from '../MatonShellNative';

export function HomeScreen(): React.JSX.Element {
  const theme = useTheme();
  const [apps, setApps] = useState<LauncherApp[]>([]);
  useEffect(() => {
    let live = true;
    void MatonShell.getLauncherApps()
      .then((items) => {
        if (live) setApps(items.slice(0, 8));
      })
      .catch(() => {});
    return () => {
      live = false;
    };
  }, []);
  const now = new Date();
  const shortcuts = apps.map((app) => (
    <Pressable
      key={app.component}
      accessibilityRole="button"
      focusable
      accessibilityLabel={`Open ${app.label}`}
      onPress={() => void MatonShell.launchApp(app.component)}
      style={({pressed}) => [
        styles.shortcut,
        {backgroundColor: theme.dark ? 'rgba(0,0,0,0.76)' : 'rgba(255,255,255,0.9)'},
        pressed && styles.pressed,
      ]}
    >
      {!!app.iconUri && <Image source={{uri: app.iconUri}} style={styles.icon} />}
      <Text numberOfLines={1} style={[styles.label, {color: theme.colors.onSurface}]}>
        {app.label}
      </Text>
    </Pressable>
  ));
  return (
    <View style={styles.home}>
      <View
        style={[
          styles.clockScrim,
          {backgroundColor: theme.dark ? 'rgba(0,0,0,0.72)' : 'rgba(255,255,255,0.88)'},
        ]}
      >
        <Text style={[styles.clock, {color: theme.colors.onSurface}]}>
          {now.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'})}
        </Text>
        <Text style={[styles.date, {color: theme.colors.onSurface}]}>
          {now.toLocaleDateString([], {weekday: 'long', month: 'long', day: 'numeric'})}
        </Text>
      </View>
      <View style={styles.shortcuts}>
        {shortcuts}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  home: {flex: 1, backgroundColor: 'transparent', padding: 44, justifyContent: 'center'},
  clockScrim: {
    alignSelf: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.58)',
    borderRadius: 22,
    paddingHorizontal: 24,
    paddingVertical: 16,
  },
  clock: {fontSize: 74, fontWeight: '300'},
  date: {fontSize: 23},
  shortcuts: {flexDirection: 'row', flexWrap: 'wrap', gap: 14, marginTop: 32},
  shortcut: {width: 96, alignItems: 'center', gap: 6, borderRadius: 12, padding: 10},
  pressed: {opacity: 0.8},
  icon: {width: 48, height: 48},
  label: {fontSize: 12},
});
