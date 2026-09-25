import React, {useCallback, useEffect, useState} from 'react';
import {FlatList, Image, StyleSheet, Text, useWindowDimensions} from 'react-native';
import {Appbar, Button, Card, IconButton, Surface, useTheme} from 'react-native-paper';
import {MatonShell, type RecentTask} from '../MatonShellNative';

export function RecentsScreen(): React.JSX.Element {
  const theme = useTheme();
  const [tasks, setTasks] = useState<RecentTask[]>([]);
  const [loading, setLoading] = useState(true);
  const columns = Math.max(2, Math.floor(useWindowDimensions().width / 300));
  const refresh = useCallback(() => {
    setLoading(true);
    void MatonShell.getRecentTasks()
      .then(setTasks)
      .catch(() => setTasks([]))
      .finally(() => setLoading(false));
  }, []);
  useEffect(() => {
    let live = true;
    void MatonShell.getRecentTasks()
      .then((items) => {
        if (live) setTasks(items);
      })
      .catch(() => {
        if (live) setTasks([]);
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, []);
  return (
    <Surface style={[styles.panel, {backgroundColor: theme.colors.surface}]}>
      <Appbar.Header>
        <Appbar.Content title="Recent apps" />
        <IconButton icon="refresh" accessibilityLabel="Refresh" onPress={refresh} />
        <IconButton icon="home" accessibilityLabel="Home" onPress={MatonShell.goHome} />
      </Appbar.Header>
      <FlatList
        data={tasks}
        key={columns}
        numColumns={columns}
        keyExtractor={(task) => `${task.taskId}`}
        contentContainerStyle={styles.grid}
        renderItem={({item}) => (
          <Card
            style={[styles.card, {backgroundColor: theme.colors.surfaceVariant}]}
            onPress={() => void MatonShell.moveTaskToFront(item.taskId)}
          >
            {!!item.thumbnailUri && (
              <Image
                source={{uri: item.thumbnailUri}}
                style={styles.thumbnail}
                resizeMode="cover"
              />
            )}
            <Card.Title
              title={item.label}
              subtitle={item.packageName}
              left={() =>
                item.iconUri ? <Image source={{uri: item.iconUri}} style={styles.icon} /> : null
              }
            />
            <Card.Actions>
              <Button
                compact
                onPress={() => {
                  void MatonShell.setTaskFullscreen(item.taskId)
                    .then(() => MatonShell.moveTaskToFront(item.taskId))
                    .catch(() => false);
                }}
              >
                Fullscreen
              </Button>
              <Button
                compact
                onPress={() => {
                  void MatonShell.closeRecentTask(item.taskId).then(refresh);
                }}
              >
                Close
              </Button>
            </Card.Actions>
          </Card>
        )}
        ListEmptyComponent={
          <Text style={styles.empty}>{loading ? 'Loading recent apps…' : 'No recent apps'}</Text>
        }
      />
    </Surface>
  );
}

const styles = StyleSheet.create({
  panel: {flex: 1, backgroundColor: 'rgba(24,28,34,0.96)'},
  grid: {padding: 18, gap: 12},
  card: {flex: 1, minWidth: 220, margin: 8, overflow: 'hidden'},
  thumbnail: {height: 180},
  icon: {width: 38, height: 38, marginLeft: 12},
  empty: {color: 'white', textAlign: 'center', padding: 36},
});
