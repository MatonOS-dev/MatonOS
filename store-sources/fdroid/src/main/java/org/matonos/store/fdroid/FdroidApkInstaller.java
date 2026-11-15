package org.matonos.store.fdroid;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

/** Verifies every downloaded APK before handing any bytes to the store. */
public final class FdroidApkInstaller {
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
