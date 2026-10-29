package org.matonos.flathub.fdroid;

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

    String fixture = "{\"packages\":{\"org.example.demo\":{\"metadata\":{\"name\":{\"en-US\":\"Demo App\"},\"summary\":{\"en-US\":\"A small fixture\"},\"icon\":{\"en-US\":{\"name\":\"/icons/demo.png\"}}},\"versions\":{\"abc\":{\"file\":[{\"name\":\"/demo.apk\",\"sha256\":\"abc123\"},{\"name\":\"/demo-split.apk\",\"sha256\":\"def456\"}],\"manifest\":{\"versionName\":\"1.2\",\"versionCode\":12}}}}}}";
    java.util.Map<String, Object> parsed = client.parseIndex(fixture);
    java.util.Map<String, Object> app = client.detailsFromParsedIndex(parsed, "org.example.demo");
    check("Demo App".equals(app.get("name")), "parser name");
    check("A small fixture".equals(app.get("summary")), "parser summary");
    check("/demo.apk".equals(app.get("apkName")), "parser APK name");
    check("abc123".equals(app.get("apkSha256")), "parser APK hash");
    check(((java.util.List<?>) app.get("apkFiles")).size() == 2, "parser split APK files");
    check(((Number) app.get("versionCode")).longValue() == 12, "parser version code");
    testApkHashBeforeCommit(signedJar.getParentFile());
    System.out.println("PASS: signed JAR, signer pin, index hash, index parser, APK hash before commit");
  }

  private static void testApkHashBeforeCommit(File directory) throws Exception {
    File apk = new File(directory, "download.apk");
    try (FileOutputStream output = new FileOutputStream(apk)) { output.write("verified APK bytes".getBytes(StandardCharsets.UTF_8)); }
    String goodHash = fingerprint(bytes(apk));
    FdroidApkInstaller installer = new FdroidApkInstaller();
    final int[] commits = {0};
    installer.verifyThenCommit(java.util.Collections.singletonList(apk), java.util.Collections.singletonList(goodHash), files -> commits[0]++);
    check(commits[0] == 1, "valid APK reaches session commit");
    try {
      installer.verifyThenCommit(java.util.Arrays.asList(apk, apk), java.util.Arrays.asList(goodHash, "00".repeat(32)), files -> commits[0]++);
      throw new AssertionError("Accepted APK hash mismatch");
    } catch (SecurityException expected) { }
    check(commits[0] == 1, "hash mismatch aborts before session commit");
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
