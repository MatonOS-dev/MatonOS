import React, {useCallback, useEffect, useRef, useState} from 'react';
import {FlatList, Image, StyleSheet, Text, useWindowDimensions} from 'react-native';
import {Appbar, Button, Card, IconButton, Surface, useTheme} from 'react-native-paper';
import {MatonOS, type RecentTask} from '@matonos/rn-common';
import {MatonRecents} from '../RecentsNative';

export function RecentsScreen(): React.JSX.Element {
  const theme = useTheme();
  const [tasks, setTasks] = useState<RecentTask[]>([]);
  const [loading, setLoading] = useState(true);
  const requestedThumbnails = useRef(new Set<number>());
  const columns = Math.max(2, Math.floor(useWindowDimensions().width / 300));
  const loadTasks = useCallback(() => {
    void MatonOS.getRecentTasks()
      .then(async (items) => {
        const visible = items.filter(
          (task) =>
            task.packageName !== 'org.matonos.recents' &&
            task.packageName !== 'org.matonos.shell' &&
            task.packageName !== 'com.android.systemui' &&
            task.packageName !== 'android',
        );
        const enriched = await Promise.all(
          visible.map(async (task) => {
            if (task.thumbnailUri || requestedThumbnails.current.has(task.taskId)) return task;
            requestedThumbnails.current.add(task.taskId);
            const thumbnailUri = await MatonOS.getRecentTaskThumbnail(task.taskId);
            return thumbnailUri ? {...task, thumbnailUri} : task;
          }),
        );
        setTasks(enriched);
      })
      .catch(() => setTasks([]))
      .finally(() => setLoading(false));
  }, []);
  useEffect(() => {
    loadTasks();
    const timer = setInterval(loadTasks, 4000);
    return () => clearInterval(timer);
  }, [loadTasks]);
  const refresh = useCallback(() => {
    setLoading(true);
    loadTasks();
  }, [loadTasks]);
  const goHome = (): void => {
    void MatonOS.navigate('home').then((ok) => {
      if (!ok) MatonRecents.goHome();
    });
  };
  return (
    <Surface style={[styles.panel, {backgroundColor: theme.colors.surface}]}>
      <Appbar.Header>
        <Appbar.Content title="Recent apps" />
        <IconButton icon="refresh" accessibilityLabel="Refresh" onPress={refresh} />
        <IconButton icon="home" accessibilityLabel="Home" onPress={goHome} />
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
            onPress={() => void MatonOS.moveTaskToFront(item.taskId)}
          >
            {!!item.thumbnailUri && (
              <Image
                source={{uri: item.thumbnailUri}}
                style={styles.thumbnail}
                resizeMode="cover"
              />
            )}
            <Card.Title
              title={item.label || item.packageName}
              subtitle={item.packageName}
              left={() =>
                item.iconUri ? <Image source={{uri: item.iconUri}} style={styles.icon} /> : null
              }
            />
            <Card.Actions>
              <Button
                compact
                onPress={() => {
                  void MatonOS.setTaskFullscreen(item.taskId).then(() =>
                    MatonOS.moveTaskToFront(item.taskId),
                  );
                }}
              >
                Fullscreen
              </Button>
              <Button
                compact
                onPress={() => {
                  void MatonOS.removeRecentTask(item.taskId).then(refresh);
                }}
              >
                Close
              </Button>
            </Card.Actions>
          </Card>
        )}
        ListEmptyComponent={
          <Text style={[styles.empty, {color: theme.colors.onSurfaceVariant}]}>
            {loading ? 'Loading recent apps…' : 'No recent apps'}
          </Text>
        }
      />
    </Surface>
  );
}

const styles = StyleSheet.create({
  panel: {flex: 1},
  grid: {padding: 18, gap: 12},
  card: {flex: 1, minWidth: 220, margin: 8, overflow: 'hidden'},
  thumbnail: {height: 180},
  icon: {width: 38, height: 38, marginLeft: 12},
  empty: {textAlign: 'center', padding: 36},
});
