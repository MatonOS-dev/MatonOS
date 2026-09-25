import React, {Component, type ErrorInfo, type PropsWithChildren} from 'react';
import {Text} from 'react-native';
import {PaperProvider} from 'react-native-paper';
import {MatonShell} from './MatonShellNative';
import {useWallpaperTheme} from './hooks/useWallpaperTheme';
import {AppDrawerScreen} from './screens/AppDrawerScreen';
import {HomeScreen} from './screens/HomeScreen';
import {RecentsScreen} from './screens/RecentsScreen';
import {Shelf} from './components/Shelf';

type RootProps = {component?: string; page?: string; surface?: string};
type BoundaryProps = PropsWithChildren<{surface: string}>;
type BoundaryState = {failed: boolean};

class SurfaceBoundary extends Component<BoundaryProps, BoundaryState> {
  state: BoundaryState = {failed: false};
  static getDerivedStateFromError(): BoundaryState {
    return {failed: true};
  }
  componentDidCatch(error: Error, info: ErrorInfo): void {
    void info;
    MatonShell.reportSurfaceFailure(this.props.surface, error.message);
  }
  render(): React.ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

export default function App(props: RootProps): React.JSX.Element {
  const theme = useWallpaperTheme();
  const page = props.page ?? (props.component === 'MatonPanel' ? 'drawer' : 'home');
  const icons: Record<string, string> = {
    'arrow-left': '←',
    'view-grid': '▦',
    'chevron-up': '⌃',
    home: '⌂',
    refresh: '↻',
    close: '×',
  };
  return (
    <PaperProvider
      theme={theme}
      settings={{
        icon: ({name, color, size}) => (
          <Text style={{color, fontSize: size, textAlign: 'center'}}>
            {icons[String(name)] ?? '•'}
          </Text>
        ),
      }}
    >
      <SurfaceBoundary surface={props.surface ?? page}>
        {props.component === 'MatonShelf' ? (
          <Shelf />
        ) : page === 'recents' ? (
          <RecentsScreen />
        ) : page === 'drawer' ? (
          <AppDrawerScreen />
        ) : (
          <HomeScreen />
        )}
      </SurfaceBoundary>
    </PaperProvider>
  );
}
