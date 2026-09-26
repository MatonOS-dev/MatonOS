import React from 'react';
import {ShellAppProvider} from './components/ShellAppProvider';
import {AppDrawerScreen} from './screens/AppDrawerScreen';

export default function DrawerApp(): React.JSX.Element {
  return (
    <ShellAppProvider surface="drawer">
      <AppDrawerScreen />
    </ShellAppProvider>
  );
}
