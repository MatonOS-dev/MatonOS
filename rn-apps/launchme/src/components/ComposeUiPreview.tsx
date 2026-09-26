import React, {useState} from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {Host, Switch} from '@expo/ui/jetpack-compose';
import {useTheme} from 'react-native-paper';

/** Small dev-only rendering check; production Home stays focused on launchable apps. */
export function ComposeUiPreview(): React.JSX.Element {
  const theme = useTheme();
  const [enabled, setEnabled] = useState(false);
  return (
    <View style={styles.row}>
      <Text style={{color: theme.colors.onSurface}}>Compose preview (development)</Text>
      <Host style={styles.host} colorScheme={theme.dark ? 'dark' : 'light'}>
        <Switch value={enabled} onCheckedChange={setEnabled} />
      </Host>
    </View>
  );
}

const styles = StyleSheet.create({
  row: {height: 52, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 24},
  host: {width: 64, height: 48},
});
