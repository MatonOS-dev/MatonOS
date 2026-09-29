import fs from "node:fs";
import path from "node:path";

const root = path.resolve(import.meta.dirname, "..");
const packageJson = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
const dependencies = { ...packageJson.dependencies, ...packageJson.devDependencies };
if (Object.keys(dependencies).some((name) => name === "react-native-paper" || name.startsWith("react-native-paper-"))) {
  throw new Error("Flathub must use Expo UI Jetpack Compose, not react-native-paper.");
}
for (const required of ["@expo/ui", "expo", "react", "react-native"]) {
  if (!dependencies[required]) throw new Error(`Missing required dependency ${required}`);
}
const moduleConfig = JSON.parse(fs.readFileSync(path.join(root, "modules/matonos-flathub/expo-module.config.json"), "utf8"));
if (!moduleConfig.android?.modules?.some((name) => name.endsWith("MatonOSExpoModule"))) {
  throw new Error("MatonOS bridge adapter is not registered for Android.");
}
const plugin = fs.readFileSync(path.join(root, "plugins/withMatonFlathub.js"), "utf8");
for (const marker of ["minSdkVersion 32", "abiFilters", "MATON_SIGNING_STORE_FILE", "matonos-client"]) {
  if (!plugin.includes(marker)) throw new Error(`Missing build configuration: ${marker}`);
}
const lock = JSON.parse(fs.readFileSync(path.join(root, "package-lock.json"), "utf8"));
if (Object.keys(lock.packages ?? {}).some((name) => name.includes("react-native-paper"))) {
  throw new Error("react-native-paper is locked transitively.");
}
const permissions = fs.readFileSync(path.join(root, "privapp-permissions-org.matonos.flathub.xml"), "utf8");
if (!permissions.includes('package="org.matonos.flathub"') || !permissions.includes('name="org.matonos.permission.SYSTEM_BRIDGE"')) {
  throw new Error("Flathub's privileged System Bridge permission is missing.");
}
console.log("Release linkage policy OK: Compose UI, registered bridge adapter, x86_64, and per-app signing.");
