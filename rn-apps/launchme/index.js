import 'expo/src/Expo.fx';
import {AppRegistry} from 'react-native';
import HomeApp from './src/HomeApp';
import DrawerApp from './src/DrawerApp';

AppRegistry.registerComponent('MatonHome', () => HomeApp);
AppRegistry.registerComponent('MatonDrawer', () => DrawerApp);
AppRegistry.registerComponent('main', () => HomeApp);
