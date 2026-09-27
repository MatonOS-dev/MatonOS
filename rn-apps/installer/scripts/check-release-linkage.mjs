import {execFileSync} from 'node:child_process';
import {existsSync, readFileSync, statSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const pkg = JSON.parse(readFileSync(path.join(root, 'package.json'), 'utf8'));
if (pkg.dependencies['react-native-paper'] || pkg.dependencies['@matonos/rn-common']) {
  throw new Error('Installer must use Expo UI Compose and must not link Paper-wrapping rn-common UI.');
}
const apk = path.join(root, 'android/app/build/outputs/apk/release/app-release.apk');
if (!existsSync(apk) || statSync(apk).size === 0) throw new Error('Release APK is missing.');
const entries = execFileSync('unzip', ['-Z1', apk], {encoding: 'utf8'}).trim().split('\n');
const dex = entries.filter(entry => /^classes(\d+)?\.dex$/.test(entry));
if (dex.length === 0) throw new Error('Release APK has no DEX files.');
const dexBytes = Buffer.concat(dex.map(file => execFileSync('unzip', ['-p', apk, file], {
  maxBuffer: 64 * 1024 * 1024,
})));
if (!dexBytes.includes(Buffer.from('expo/modules/ui'))) {
  throw new Error('Release DEX is missing Expo UI components.');
}
if (entries.some(entry => entry.startsWith('lib/') && !entry.startsWith('lib/x86_64/'))) {
  throw new Error('Release APK contains a native ABI other than x86_64.');
}
console.log('Installer release linkage OK: Expo UI Compose and x86_64 app libraries are present.');
