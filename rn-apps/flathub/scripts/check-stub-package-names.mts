import fs from "node:fs";
import path from "node:path";
import { stubPackageFor } from "../src/sources/flatpak/StubPackage.ts";

const vector = JSON.parse(fs.readFileSync(path.join(import.meta.dirname, "../src/sources/flatpak/stub-package-names.json"), "utf8"));
let failures = 0;
for (const { ref, package: expected } of vector.valid) {
  const actual = stubPackageFor(ref);
  if (actual !== expected) { console.error(`${ref}: expected ${expected}, got ${actual}`); failures++; }
}
for (const ref of vector.invalid) {
  try { stubPackageFor(ref); console.error(`accepted invalid ref ${ref}`); failures++; } catch { /* expected */ }
}
if (failures) process.exit(1);
console.log(`stub package names: ${vector.valid.length + vector.invalid.length} vectors ok`);
