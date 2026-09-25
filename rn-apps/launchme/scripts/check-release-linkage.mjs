import {execFileSync} from 'node:child_process';
import {readFileSync, existsSync, statSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const generated = path.join(
  root,
  'android/app/build/generated/autolinking/src/main/java/com/facebook/react/PackageList.java',
);
const autolinking = path.join(root, 'android/build/generated/autolinking/autolinking.json');
const apk = path.join(root, 'android/app/build/outputs/apk/release/app-release.apk');

for (const file of [generated, autolinking, apk]) {
  if (!existsSync(file) || statSync(file).size === 0) {
    throw new Error(`Missing release native-linkage input: ${path.relative(root, file)}`);
  }
}

const packageList = readFileSync(generated, 'utf8');
if (!packageList.includes('new com.th3rdwave.safeareacontext.SafeAreaContextPackage()')) {
  throw new Error('Generated React Native PackageList does not register SafeAreaContextPackage');
}

const config = JSON.parse(readFileSync(autolinking, 'utf8'));
const safeArea = config.dependencies?.['react-native-safe-area-context']?.platforms?.android;
if (!safeArea?.packageInstance?.includes('SafeAreaContextPackage')) {
  throw new Error('Expo/RN autolinking metadata does not contain the Android safe-area package');
}

const entries = execFileSync('unzip', ['-Z1', apk], {encoding: 'utf8'}).trim().split('\n');
const dexFiles = entries.filter((entry) => /^classes(\d+)?\.dex$/.test(entry));
if (dexFiles.length === 0) throw new Error('Release APK has no DEX files');
const dexBytes = execFileSync('unzip', ['-p', apk, ...dexFiles], {maxBuffer: 64 * 1024 * 1024});
for (const className of [
  'com/th3rdwave/safeareacontext/SafeAreaContextPackage',
  'RNCSafeAreaProvider',
]) {
  if (!dexBytes.includes(Buffer.from(className))) {
    throw new Error(`Release DEX is missing ${className}`);
  }
}
if (!entries.includes('lib/x86_64/libreact_codegen_safeareacontext.so')) {
  throw new Error('Release APK is missing the safe-area Fabric codegen library for x86_64');
}

console.log('Release linkage OK: safe-area package, RNCSafeAreaProvider and Fabric library are present.');
