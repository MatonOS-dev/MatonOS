import React, {Component, type ErrorInfo, type PropsWithChildren} from 'react';
import {Text} from 'react-native';
import {PaperProvider} from 'react-native-paper';
import {MatonShelf} from './ShelfNative';
import {Shelf} from './components/Shelf';
import {useWallpaperTheme} from './hooks/useWallpaperTheme';

type BoundaryProps = PropsWithChildren;
type BoundaryState = {failed: boolean};

class ShelfBoundary extends Component<BoundaryProps, BoundaryState> {
  state: BoundaryState = {failed: false};

  static getDerivedStateFromError(): BoundaryState {
    return {failed: true};
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    void info;
    MatonShelf.reportSurfaceFailure(error.message);
  }

  render(): React.ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

export default function App(): React.JSX.Element {
  const theme = useWallpaperTheme();
  return (
    <PaperProvider
      theme={theme}
      settings={{
        icon: ({name, color, size}) => (
          <Text style={{color, fontSize: size, textAlign: 'center'}}>
            {{
              'arrow-left': '←',
              'view-grid': '▦',
              'chevron-up': '⌃',
              home: '⌂',
              refresh: '↻',
              close: '×',
            }[String(name)] ?? '•'}
          </Text>
        ),
      }}
    >
      <ShelfBoundary>
        <Shelf />
      </ShelfBoundary>
    </PaperProvider>
  );
}
