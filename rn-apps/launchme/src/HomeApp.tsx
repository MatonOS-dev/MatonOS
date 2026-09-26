import React from 'react';
import {ShellAppProvider} from './components/ShellAppProvider';
import {HomeScreen} from './screens/HomeScreen';

export default function HomeApp(): React.JSX.Element {
  return (
    <ShellAppProvider surface="home">
      <HomeScreen />
    </ShellAppProvider>
  );
}
