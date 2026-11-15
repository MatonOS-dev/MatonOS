package org.matonos.store.fdroid;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class FdroidIndexClientTest {
  public static void main(String[] args) throws Exception {
    File signedJar = new File(args[0]);
    File certFile = new File(args[1]);
    String fingerprint = fingerprint(CertificateFactory.getInstance("X.509").generateCertificate(new FileInputStream(certFile)).getEncoded());
    FdroidIndexClient client = new FdroidIndexClient();
    client.verifyEntryJar(bytes(signedJar), fingerprint);
    expectFailure(() -> client.verifyEntryJar(bytes(signedJar), "00".repeat(32)), "wrong fingerprint");

    File damaged = new File(signedJar.getParentFile(), "damaged.jar");
    damageSignedContent(signedJar, damaged);
    expectFailure(() -> client.verifyEntryJar(bytes(damaged), fingerprint), "damaged JAR signature");
    expectFailure(() -> client.verifyIndexHash("index data".getBytes(StandardCharsets.UTF_8), "00".repeat(32)), "index hash mismatch");

    String fixture = "{\"packages\":{\"org.example.demo\":{\"metadata\":{\"name\":{\"en-US\":\"Demo App\"},\"summary\":{\"en-US\":\"A small fixture\"},\"icon\":{\"en-US\":{\"name\":\"/icons/demo.png\"}}},\"versions\":{\"abc\":{\"file\":[{\"name\":\"/demo.apk\",\"sha256\":\"abc123\"},{\"name\":\"/demo-split.apk\",\"sha256\":\"def456\"}],\"manifest\":{\"versionName\":\"1.2\",\"versionCode\":12,\"signer\":{\"sha256\":[\"aa11\"]}}}}}}}";
    java.util.Map<String, Object> parsed = client.parseIndex(fixture);
    java.util.Map<String, Object> app = client.detailsFromParsedIndex(parsed, "org.example.demo");
    check("Demo App".equals(app.get("name")), "parser name");
    check("A small fixture".equals(app.get("summary")), "parser summary");
    check("/demo.apk".equals(app.get("apkName")), "parser APK name");
    check("abc123".equals(app.get("apkSha256")), "parser APK hash");
    check(((java.util.List<?>) app.get("apkFiles")).size() == 2, "parser split APK files");
    check(((Number) app.get("versionCode")).longValue() == 12, "parser version code");
    testApkHashBeforeCommit(signedJar.getParentFile());
    testUpdateEligibility(client, parsed);
    testRepositoryChanges();
    testCompatibility();
    testSafety();
    testManifest(signedJar.getParentFile());
    System.out.println("PASS: signed JAR, signer pin, index hash, index parser, APK hash before commit, update signer/version checks, repository trust/removal");
  }

  private static void testCompatibility() {
    FdroidIndexClient client = new FdroidIndexClient(37, java.util.List.of("x86_64"), java.util.Set.of("android.hardware.camera"));
    String versions = "{\"packages\":{\"p\":{\"versions\":{"
      + "\"good\":{\"file\":{\"name\":\"base.apk\",\"sha256\":\"abc\"},\"manifest\":{\"versionCode\":1,\"usesSdk\":{\"minSdkVersion\":37},\"nativecode\":[\"x86_64\"]}},"
      + "\"badSdk\":{\"file\":{\"name\":\"base.apk\",\"sha256\":\"abc\"},\"manifest\":{\"versionCode\":3,\"usesSdk\":{\"minSdkVersion\":38}}},"
      + "\"badAbi\":{\"file\":{\"name\":\"base.apk\",\"sha256\":\"abc\"},\"manifest\":{\"versionCode\":2,\"nativecode\":[\"arm64-v8a\"]}}}}}}";
    check(((Number)client.detailsFromParsedIndex(client.parseIndex(versions), "p").get("versionCode")).intValue() == 1, "newest compatible version");
  }
  private static void testSafety() {
    java.util.Map<String,Object> app = new java.util.LinkedHashMap<>();
    app.put("rawAntiFeatures", java.util.List.of(java.util.Map.of("id", "Tracking", "reason", "Reports location")));
    FdroidSafety.apply(app);
    check("potentially-unsafe".equals(((java.util.Map<?,?>)app.get("safety")).get("level")), "tracking is dangerous");
    app.put("rawAntiFeatures", java.util.List.of(java.util.Map.of("id", "UnknownFeature", "reason", "")));
    FdroidSafety.apply(app);
    check("probably-safe".equals(((java.util.Map<?,?>)app.get("safety")).get("level")), "unknown anti feature warns");
  }
  private static void testManifest(File directory) throws Exception {
    // Real aapt2-produced manifests, including a standalone split.
    for (String name : new String[]{"base", "split"}) {
      File apk = new File(directory, name + ".apk");
      if (!apk.isFile()) throw new AssertionError("Missing aapt2 fixture " + apk);
      check((name.equals("base") ? "base.apk" : "split_config.en.apk").equals(ApkManifest.sessionName(apk, "org.example.demo")), "manifest " + name);
      try { ApkManifest.sessionName(apk, "org.example.other"); throw new AssertionError("Accepted wrong package"); } catch (java.io.IOException expected) { }
    }
  }

  private static void testRepositoryChanges() throws Exception {
    String fingerprint = "ab".repeat(32);
    check(fingerprint.equals(FdroidRepository.normalizeFingerprint(fingerprint.toUpperCase(Locale.ROOT))), "repository fingerprint is normalized");
    expectIllegalArgument(() -> FdroidRepository.normalizeFingerprint(""), "fingerprint is required");
    check("xn--bcher-kva.example".equals(FdroidRepository.displayHost("https://bücher.example/repo")), "non-ASCII host is shown as punycode");
    java.util.Map<String, Object> removedRepoApp = new java.util.LinkedHashMap<>();
    removedRepoApp.put("id", "org.example.removed"); removedRepoApp.put("repository", "https://repo.example/fdroid");
    check(FdroidRepository.visibleApps(java.util.Collections.singletonList(removedRepoApp), java.util.Collections.emptySet()).isEmpty(),
        "removed repository apps disappear from search results");
  }

  private static void testUpdateEligibility(FdroidIndexClient client, java.util.Map<String, Object> index) {
    java.util.Map<String, Object> installed = new java.util.LinkedHashMap<>();
    installed.put("packageName", "org.example.demo"); installed.put("versionCode", 10L);
    installed.put("signerSha256", java.util.Collections.singletonList("aa11"));
    check(client.updatesFromParsedIndex(index, java.util.Collections.singletonList(installed)).size() == 1,
        "higher version with matching signer is offered");
    installed.put("signerSha256", java.util.Collections.singletonList("bb22"));
    check(client.updatesFromParsedIndex(index, java.util.Collections.singletonList(installed)).isEmpty(),
        "signer mismatch does not offer update");
    installed.put("signerSha256", java.util.Collections.singletonList("aa11")); installed.put("versionCode", 12L);
    check(client.updatesFromParsedIndex(index, java.util.Collections.singletonList(installed)).isEmpty(),
        "same version does not offer update");
  }

  private static void testApkHashBeforeCommit(File directory) throws Exception {
    File apk = new File(directory, "download.apk");
    try (FileOutputStream output = new FileOutputStream(apk)) { output.write("verified APK bytes".getBytes(StandardCharsets.UTF_8)); }
    String goodHash = fingerprint(bytes(apk));
    FdroidApkInstaller installer = new FdroidApkInstaller();
    installer.verify(apk, goodHash);
    expectFailure(() -> installer.verify(apk, "00".repeat(32)), "APK hash mismatch");
  }

  private static void damageSignedContent(File original, File damaged) throws Exception {
    try (ZipInputStream in = new ZipInputStream(new FileInputStream(original)); ZipOutputStream out = new ZipOutputStream(new FileOutputStream(damaged))) {
      ZipEntry entry;
      while ((entry = in.getNextEntry()) != null) {
        byte[] data = read(in);
        if (entry.getName().equals("entry.json")) data = "{\"changed\":true}".getBytes(StandardCharsets.UTF_8);
        out.putNextEntry(new ZipEntry(entry.getName())); out.write(data); out.closeEntry();
      }
    }
  }
  private interface Checked { void run() throws Exception; }
  private static void expectFailure(Checked action, String label) throws Exception {
    try { action.run(); throw new AssertionError("Accepted " + label); } catch (SecurityException expected) { }
  }
  private static void expectIllegalArgument(Checked action, String label) throws Exception {
    try { action.run(); throw new AssertionError("Accepted " + label); } catch (IllegalArgumentException expected) { }
  }
  private static void check(boolean condition, String label) { if (!condition) throw new AssertionError("Failed " + label); }
  private static byte[] bytes(File file) throws Exception { try (FileInputStream in = new FileInputStream(file)) { return read(in); } }
  private static byte[] read(java.io.InputStream in) throws Exception {
    ByteArrayOutputStream out = new ByteArrayOutputStream(); byte[] buffer = new byte[4096]; int count;
    while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count); return out.toByteArray();
  }
  private static String fingerprint(byte[] data) throws Exception {
    StringBuilder out = new StringBuilder();
    for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) out.append(String.format(Locale.ROOT, "%02x", b & 0xff));
    return out.toString();
  }
}
