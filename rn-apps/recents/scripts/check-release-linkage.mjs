import {execFileSync} from 'node:child_process';
import {existsSync, readFileSync, statSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const generated = path.join(root, 'android/app/build/generated/autolinking/src/main/java/com/facebook/react/PackageList.java');
const autolinking = path.join(root, 'android/build/generated/autolinking/autolinking.json');
const modules = path.join(root, 'node_modules/expo/android/build/generated/expo/src/main/java/expo/modules/ExpoModulesPackageList.kt');
const apk = path.join(root, 'android/app/build/outputs/apk/release/app-release.apk');

for (const file of [generated, autolinking, modules, apk]) {
  if (!existsSync(file) || statSync(file).size === 0) {
    throw new Error(`Missing release native-linkage input: ${path.relative(root, file)}`);
  }
}
const packageList = readFileSync(generated, 'utf8');
if (!packageList.includes('new com.th3rdwave.safeareacontext.SafeAreaContextPackage()')) {
  throw new Error('Generated React Native PackageList does not register SafeAreaContextPackage');
}
const config = JSON.parse(readFileSync(autolinking, 'utf8'));
if (!config.dependencies?.['react-native-safe-area-context']?.platforms?.android?.packageInstance?.includes('SafeAreaContextPackage')) {
  throw new Error('React Native autolinking metadata does not include the safe-area package');
}
const moduleList = readFileSync(modules, 'utf8');
if (!moduleList.includes('org.matonos.recents.MatonRecentsExpoModule::class.java')) {
  throw new Error('Expo module registry does not include MatonRecentsExpoModule');
}

const entries = execFileSync('unzip', ['-Z1', apk], {encoding: 'utf8'}).trim().split('\n');
const dexFiles = entries.filter((entry) => /^classes(\d+)?\.dex$/.test(entry));
if (dexFiles.length === 0) throw new Error('Release APK has no DEX files');
const dexBytes = Buffer.concat(dexFiles.map((file) => execFileSync('unzip', ['-p', apk, file], {maxBuffer: 64 * 1024 * 1024})));
for (const className of [
  'com/th3rdwave/safeareacontext/SafeAreaContextPackage',
  'RNCSafeAreaProvider',
  'org/matonos/recents/MatonRecentsExpoModule',
  'androidx/appcompat/app/AppCompatActivity',
]) {
  if (!dexBytes.includes(Buffer.from(className))) throw new Error(`Release DEX is missing ${className}`);
}
if (!entries.includes('lib/x86_64/libreact_codegen_safeareacontext.so')) {
  throw new Error('Release APK is missing the safe-area Fabric library');
}
if (entries.some((entry) => entry.startsWith('lib/') && !entry.startsWith('lib/x86_64/'))) {
  throw new Error('Release APK contains a native ABI other than x86_64');
}
console.log('Release linkage OK: Expo Recents module, AppCompat, safe-area provider, Hermes app and x86_64 native libraries are present.');
