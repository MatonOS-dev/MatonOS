import React, {Component, type ErrorInfo, type PropsWithChildren} from 'react';
import {Text} from 'react-native';
import {PaperProvider} from 'react-native-paper';
import {MatonShell} from '../MatonShellNative';
import {useWallpaperTheme} from '../hooks/useWallpaperTheme';

type Props = PropsWithChildren<{surface: string}>;
type State = {failed: boolean};

class SurfaceBoundary extends Component<Props, State> {
  state: State = {failed: false};

  static getDerivedStateFromError(): State {
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

export function ShellAppProvider({surface, children}: Props): React.JSX.Element {
  const theme = useWallpaperTheme();
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
      <SurfaceBoundary surface={surface}>{children}</SurfaceBoundary>
    </PaperProvider>
  );
}
