package org.matonos.flathub.fdroid;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Verifies every downloaded APK before handing any bytes to PackageInstaller. */
public final class FdroidApkInstaller {
  public interface CommitAction { void commit(List<File> verifiedApks) throws Exception; }

  public void verifyThenCommit(List<File> apks, List<String> hashes, CommitAction action) throws Exception {
    if (apks.isEmpty() || apks.size() != hashes.size()) throw new IllegalArgumentException("APK files and hashes do not match");
    for (int i = 0; i < apks.size(); i++) verify(apks.get(i), hashes.get(i));
    action.commit(new ArrayList<>(apks));
  }

  public void verify(File apk, String expectedHash) throws Exception {
    if (expectedHash == null || !expectedHash.matches("(?i)[0-9a-f]{64}")) throw new IllegalArgumentException("Invalid F-Droid APK SHA-256");
    MessageDigest digest = MessageDigest.getInstance("SHA-256");
    try (InputStream input = new FileInputStream(apk)) {
      byte[] buffer = new byte[16384];
      int count;
      while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
    }
    StringBuilder actual = new StringBuilder();
    for (byte value : digest.digest()) actual.append(String.format(Locale.ROOT, "%02x", value & 0xff));
    if (!actual.toString().equalsIgnoreCase(expectedHash)) throw new SecurityException("F-Droid APK SHA-256 mismatch");
  }
}
