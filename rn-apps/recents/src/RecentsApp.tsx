import React, {Component, type ErrorInfo, type PropsWithChildren} from 'react';
import {Text} from 'react-native';
import {PaperProvider} from 'react-native-paper';
import {useMatonTheme} from '@matonos/rn-common';
import {MatonRecents} from './RecentsNative';
import {RecentsScreen} from './screens/RecentsScreen';

type BoundaryProps = PropsWithChildren;
type BoundaryState = {failed: boolean};

class SurfaceBoundary extends Component<BoundaryProps, BoundaryState> {
  state: BoundaryState = {failed: false};
  static getDerivedStateFromError(): BoundaryState {
    return {failed: true};
  }
  componentDidCatch(error: Error, info: ErrorInfo): void {
    void info;
    MatonRecents.reportSurfaceFailure(error.message);
  }
  render(): React.ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

export default function RecentsApp(): React.JSX.Element {
  const theme = useMatonTheme();
  return (
    <PaperProvider
      theme={theme}
      settings={{
        icon: ({name, color, size}) => (
          <Text style={{color, fontSize: size, textAlign: 'center'}}>
            {({home: '⌂', refresh: '↻', close: '×'} as Record<string, string>)[String(name)] ?? '•'}
          </Text>
        ),
      }}
    >
      <SurfaceBoundary>
        <RecentsScreen />
      </SurfaceBoundary>
    </PaperProvider>
  );
}
