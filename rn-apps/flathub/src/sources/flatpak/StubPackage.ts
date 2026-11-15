// Stub Android package name for a Flatpak ref. Must match the subsystem's
// DeployManifest.packageFor; stub-package-names.json is the shared vector both
// repos test against (npm run test:stub-names here, StubPackageNameVectorTest there).

const SEGMENT = /^[A-Za-z0-9._-]+$/;

export function validRef(ref: string): boolean {
  const parts = ref.split("/");
  return parts.length === 4 && parts.every((part) => SEGMENT.test(part) && part !== "." && part !== "..");
}

export function stubPackageFor(ref: string): string {
  if (!validRef(ref)) throw new Error(`Invalid Flatpak ref: ${ref}`);
  const segments = ref.split("/").map((part) => (/^[A-Za-z]/.test(part) ? "" : "v") + part.replace(/[^A-Za-z0-9_]/g, "_"));
  const name = ["flatpak", ...segments].join(".");
  if (name.length > 255) throw new Error("Flatpak ref is too long for an Android package name");
  return name;
}
