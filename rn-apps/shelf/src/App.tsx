import React, {
  Component,
  useCallback,
  useEffect,
  useRef,
  type ErrorInfo,
  type PropsWithChildren,
} from 'react';
import {MatonShelf} from './ShelfNative';
import {useWallpaperTheme} from './hooks/useWallpaperTheme';
import {Actions} from './components/Actions';
import {useShelfExpanded} from './hooks/useShelfExtended';
import {Pressable, View} from 'react-native';

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
  const {expanded, hovered} = useShelfExpanded();

  const timer = useRef(null);

  const onInteract = useCallback(() => {
    if (timer.current) clearInterval(timer.current);
    MatonShelf.setShelfExpanded(true);
    console.log('enter');
  }, [expanded, hovered]);

  useEffect(() => {
    if (!hovered) {
      const timer = setTimeout(() => MatonShelf.setShelfExpanded(false), 1000);
      return () => clearTimeout(timer);
    }
    MatonShelf.setShelfExpanded(true);
  }, [hovered]);

  return (
    <ShelfBoundary>
      <View
        onLayout={(e) => {
          const {x, y, width, height} = e.nativeEvent.layout;
          MatonShelf.setTouchableRect(x, y, width, height);
        }}
      >
        <Actions />
      </View>
    </ShelfBoundary>
  );
}
