import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '..');
const packageJson = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const allDeps = {...packageJson.dependencies, ...packageJson.devDependencies};
if (Object.keys(allDeps).some((name) => name === 'react-native-paper' || name.startsWith('react-native-paper-'))) {
  throw new Error('Settings must not include react-native-paper.');
}
const nativeManifest = fs.readFileSync(path.join(root, 'modules/matonos-settings/expo-module.config.json'), 'utf8');
for (const name of ['MatonOSExpoModule', 'SystemIconModule', 'HardwareModule']) {
  if (!nativeManifest.includes(name)) throw new Error(`Missing local Expo module ${name}`);
}
for (const required of ['expo-splash-screen', '@expo/ui', 'react-native']) {
  if (!allDeps[required]) throw new Error(`Missing required dependency ${required}`);
}
const lock = JSON.parse(fs.readFileSync(path.join(root, 'package-lock.json'), 'utf8'));
if (Object.keys(lock.packages ?? {}).some((name) => name.includes('react-native-paper'))) throw new Error('react-native-paper is locked transitively.');
console.log('Release linkage policy OK: Compose UI only; splash and local native modules are present.');
