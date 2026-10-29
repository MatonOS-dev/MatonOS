package org.matonos.flathub.fdroid;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.cert.Certificate;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.jar.JarEntry;
import java.util.jar.JarInputStream;

/** Reads and caches one F-Droid index-v2 catalogue after checking its signed entry point. */
public final class FdroidIndexClient {
  // Official repository index signer; source: https://f-droid.org/docs/Release_Channels_and_Signing_Keys/
  public static final String OFFICIAL_FINGERPRINT = "43238d512c1e5eb2d6569f4a3afbf5523418b82e0a3ed1552770abb9a9c9ccab";
  private static final String DEFAULT_REPO = "https://f-droid.org/repo";

  public List<Map<String, Object>> search(File cacheRoot, String repository, String query) throws Exception {
    return search(cacheRoot, repository, OFFICIAL_FINGERPRINT, query);
  }

  public List<Map<String, Object>> search(File cacheRoot, String repository, String fingerprint, String query) throws Exception {
    Map<String, Object> catalogue = load(cacheRoot, repository, fingerprint);
    Object packagesValue = catalogue.get("packages");
    if (!(packagesValue instanceof Map)) return Collections.emptyList();
    String needle = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
    List<Map<String, Object>> apps = new ArrayList<>();
    for (Map.Entry<?, ?> entry : ((Map<?, ?>) packagesValue).entrySet()) {
      Map<String, Object> app = app(String.valueOf(entry.getKey()), map(entry.getValue()));
      if (app != null && (needle.isEmpty() || (text(app.get("name")) + " " + text(app.get("summary")) + " " + text(app.get("id"))).toLowerCase(Locale.ROOT).contains(needle))) apps.add(app);
    }
    apps.sort(Comparator.comparing(a -> text(a.get("name")).toLowerCase(Locale.ROOT)));
    return apps;
  }

  public Map<String, Object> details(File cacheRoot, String repository, String packageName) throws Exception {
    return details(cacheRoot, repository, OFFICIAL_FINGERPRINT, packageName);
  }

  public Map<String, Object> details(File cacheRoot, String repository, String fingerprint, String packageName) throws Exception {
    Map<String, Object> packages = map(load(cacheRoot, repository, fingerprint).get("packages"));
    return app(packageName, map(packages.get(packageName)));
  }

  public List<Map<String, Object>> installedCatalogue(File cacheRoot, String repository, List<String> installedPackages) throws Exception {
    return installedCatalogue(cacheRoot, repository, OFFICIAL_FINGERPRINT, installedPackages);
  }

  public List<Map<String, Object>> installedCatalogue(File cacheRoot, String repository, String fingerprint, List<String> installedPackages) throws Exception {
    Map<String, Object> packages = map(load(cacheRoot, repository, fingerprint).get("packages"));
    List<Map<String, Object>> installed = new ArrayList<>();
    for (String packageName : installedPackages) {
      Map<String, Object> value = app(packageName, map(packages.get(packageName)));
      if (value != null) installed.add(value);
    }
    installed.sort(Comparator.comparing(a -> text(a.get("name")).toLowerCase(Locale.ROOT)));
    return installed;
  }

  public List<Map<String, Object>> updatesCatalogue(File cacheRoot, String repository, List<Map<String, Object>> installedPackages) throws Exception {
    return updatesCatalogue(cacheRoot, repository, OFFICIAL_FINGERPRINT, installedPackages);
  }

  public List<Map<String, Object>> updatesCatalogue(File cacheRoot, String repository, String fingerprint, List<Map<String, Object>> installedPackages) throws Exception {
    return updatesFromParsedIndex(load(cacheRoot, repository, fingerprint), installedPackages);
  }

  List<Map<String, Object>> updatesFromParsedIndex(Map<String, Object> catalogue, List<Map<String, Object>> installedPackages) {
    catalogue = map(catalogue.get("packages"));
    List<Map<String, Object>> updates = new ArrayList<>();
    for (Map<String, Object> installed : installedPackages) {
      String packageName = text(installed.get("packageName"));
      Map<String, Object> app = app(packageName, map(catalogue.get(packageName)));
      if (app == null || number(app.get("versionCode")) <= number(installed.get("versionCode"))) continue;
      if (!sameSigners(strings(installed.get("signerSha256")), strings(app.get("signerSha256")))) continue;
      updates.add(app);
    }
    updates.sort(Comparator.comparing(a -> text(a.get("name")).toLowerCase(Locale.ROOT)));
    return updates;
  }

  Map<String, Object> parseIndex(String json) {
    Object parsed = Json.parse(json);
    if (!(parsed instanceof Map)) throw new IllegalArgumentException("F-Droid index is not an object");
    return cast(parsed);
  }

  Map<String, Object> detailsFromParsedIndex(Map<String, Object> index, String packageName) {
    return app(packageName, map(map(index.get("packages")).get(packageName)));
  }

  void verifyEntryJar(byte[] jarBytes, String expectedFingerprint) throws Exception {
    boolean foundContent = false;
    try (JarInputStream jar = new JarInputStream(new ByteArrayInputStream(jarBytes), true)) {
      JarEntry entry;
      byte[] buffer = new byte[8192];
      while ((entry = jar.getNextJarEntry()) != null) {
        if (entry.isDirectory() || entry.getName().startsWith("META-INF/")) continue;
        while (jar.read(buffer) != -1) { }
        Certificate[] certificates = entry.getCertificates();
        if (certificates == null || certificates.length == 0) throw new SecurityException("Unsigned entry in F-Droid entry JAR");
        boolean signerMatched = false;
        for (Certificate certificate : certificates) {
          if (fingerprint(certificate.getEncoded()).equals(normalizeFingerprint(expectedFingerprint))) signerMatched = true;
        }
        if (!signerMatched) throw new SecurityException("F-Droid entry JAR signer fingerprint did not match");
        foundContent = true;
      }
    }
    if (!foundContent) throw new SecurityException("F-Droid entry JAR has no signed content");
  }

  void verifyIndexHash(byte[] index, String expectedHash) throws Exception {
    if (!fingerprint(index).equals(normalizeFingerprint(expectedHash))) throw new SecurityException("F-Droid index SHA-256 mismatch");
  }

  private Map<String, Object> load(File cacheRoot, String repository, String expectedFingerprint) throws Exception {
    String repo = (repository == null || repository.isEmpty()) ? DEFAULT_REPO : repository;
    String signer = FdroidRepository.normalizeFingerprint(expectedFingerprint);
    File repoCache = new File(cacheRoot, fingerprint((repo + "\n" + signer).getBytes("UTF-8")));
    File indexFile = new File(repoCache, "index-v2.json");
    byte[] entryJar = download(repo + "/entry.jar");
    verifyEntryJar(entryJar, signer);
    byte[] entryBytes = verifiedEntryJson(entryJar);
    Map<String, Object> entry = parseIndex(new String(entryBytes, "UTF-8"));
    Map<String, Object> indexInfo = map(entry.get("index"));
    String expectedHash = text(indexInfo.get("sha256"));
    if (expectedHash.isEmpty()) throw new IllegalArgumentException("F-Droid entry has no index hash");
    if (indexFile.isFile()) {
      byte[] cached;
      try (InputStream input = new java.io.FileInputStream(indexFile)) { cached = readAll(input); }
      if (fingerprint(cached).equals(normalizeFingerprint(expectedHash))) return parseIndex(new String(cached, "UTF-8"));
    }
    byte[] index = download(repo + "/index-v2.json");
    verifyIndexHash(index, expectedHash);
    if (!repoCache.exists() && !repoCache.mkdirs()) throw new IllegalStateException("Cannot create F-Droid cache");
    try (FileOutputStream output = new FileOutputStream(indexFile)) { output.write(index); }
    return parseIndex(new String(index, "UTF-8"));
  }

  private byte[] verifiedEntryJson(byte[] jarBytes) throws Exception {
    try (JarInputStream jar = new JarInputStream(new ByteArrayInputStream(jarBytes), true)) {
      JarEntry entry;
      while ((entry = jar.getNextJarEntry()) != null) if ("entry.json".equals(entry.getName())) return readAll(jar);
    }
    throw new IllegalArgumentException("F-Droid entry JAR does not contain entry.json");
  }

  private byte[] download(String address) throws Exception {
    HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
    connection.setConnectTimeout(20000); connection.setReadTimeout(60000); connection.setInstanceFollowRedirects(true);
    connection.setRequestProperty("User-Agent", "MatonOS-Software-Center/1.0");
    try {
      if (connection.getResponseCode() < 200 || connection.getResponseCode() >= 300) throw new IllegalStateException("F-Droid returned HTTP " + connection.getResponseCode());
      try (InputStream input = connection.getInputStream()) { return readAll(input); }
    } finally { connection.disconnect(); }
  }

  private Map<String, Object> app(String packageName, Map<String, Object> raw) {
    Map<String, Object> metadata = map(raw.get("metadata"));
    Map<String, Object> versions = map(raw.get("versions"));
    if (versions.isEmpty()) return null;
    Map<String, Object> latest = null;
    for (Object value : versions.values()) {
      Map<String, Object> version = map(value);
      if (latest == null || number(map(version.get("manifest")).get("versionCode")) > number(map(latest.get("manifest")).get("versionCode"))) latest = version;
    }
    if (latest == null) return null;
    Map<String, Object> manifest = map(latest.get("manifest"));
    Object rawFile = latest.get("file");
    List<Map<String, Object>> files = new ArrayList<>();
    if (rawFile instanceof List) {
      for (Object value : (List<?>) rawFile) files.add(map(value));
    } else if (rawFile instanceof Map) {
      files.add(map(rawFile));
    }
    if (files.isEmpty()) return null;
    Map<String, Object> file = files.get(0);
    List<Map<String, String>> apkFiles = new ArrayList<>();
    for (Map<String, Object> item : files) {
      String name = text(item.get("name"));
      String sha256 = text(item.get("sha256"));
      if (!name.isEmpty() && !sha256.isEmpty()) {
        Map<String, String> apk = new LinkedHashMap<>();
        apk.put("name", name); apk.put("sha256", sha256); apkFiles.add(apk);
      }
    }
    if (apkFiles.isEmpty()) return null;
    String icon = localized(metadata.get("icon"));
    Map<String, Object> result = new LinkedHashMap<>();
    result.put("id", packageName); result.put("packageName", packageName); result.put("name", fallback(localized(metadata.get("name")), packageName));
    result.put("summary", localized(metadata.get("summary"))); result.put("icon", icon.isEmpty() ? "" : absolutePath(icon));
    result.put("developer", ""); result.put("ref", packageName); result.put("description", localized(metadata.get("description")));
    result.put("screenshots", Collections.emptyList()); result.put("permissions", Collections.emptyList());
    result.put("versionName", text(manifest.get("versionName"))); result.put("versionCode", number(manifest.get("versionCode")));
    result.put("signerSha256", strings(map(manifest.get("signer")).get("sha256")));
    result.put("apkName", text(file.get("name"))); result.put("apkSha256", text(file.get("sha256"))); result.put("apkFiles", apkFiles);
    result.put("repository", "");
    return result;
  }

  private String absolutePath(String path) { return DEFAULT_REPO + (path.startsWith("/") ? path : "/" + path); }
  private static String localized(Object value) {
    if (value instanceof String) return (String) value;
    Map<String, Object> values = map(value);
    Object localized = values.get("en-US"); if (localized == null) localized = values.get("en");
    if (localized == null && !values.isEmpty()) localized = values.values().iterator().next();
    if (localized instanceof Map) localized = map(localized).get("name");
    String result = text(localized);
    return result;
  }
  private static String fallback(String value, String other) { return value.isEmpty() ? other : value; }
  private static Map<String, Object> map(Object value) { return value instanceof Map ? cast(value) : Collections.emptyMap(); }
  @SuppressWarnings("unchecked") private static Map<String, Object> cast(Object value) { return (Map<String, Object>) value; }
  private static String text(Object value) { return value == null ? "" : String.valueOf(value); }
  private static List<String> strings(Object value) {
    List<String> result = new ArrayList<>();
    if (value instanceof List) for (Object item : (List<?>) value) result.add(normalizeFingerprint(text(item)));
    else if (value instanceof String && !((String) value).isEmpty()) result.add(normalizeFingerprint((String) value));
    Collections.sort(result);
    return result;
  }
  private static boolean sameSigners(List<String> installed, List<String> repository) {
    return !installed.isEmpty() && installed.equals(repository);
  }
  private static long number(Object value) { return value instanceof Number ? ((Number) value).longValue() : 0; }
  private static String normalizeFingerprint(String value) { return value == null ? "" : value.replace(":", "").replace(" ", "").toLowerCase(Locale.ROOT); }
  private static String fingerprint(byte[] bytes) throws Exception {
    byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes); StringBuilder out = new StringBuilder();
    for (byte b : hash) out.append(String.format(Locale.ROOT, "%02x", b & 0xff)); return out.toString();
  }
  private static byte[] readAll(InputStream input) throws Exception {
    ByteArrayOutputStream output = new ByteArrayOutputStream(); byte[] buffer = new byte[16384]; int read;
    while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read); return output.toByteArray();
  }
}
