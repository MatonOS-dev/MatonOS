package org.matonos.flathub.fdroid;

import java.net.IDN;
import java.net.URI;
import java.util.Locale;
import java.util.List;
import java.util.Map;
import java.util.ArrayList;
import java.util.Set;

/** Validation and display helpers for explicitly trusted F-Droid repositories. */
public final class FdroidRepository {
  private FdroidRepository() { }

  public static String normalizeFingerprint(String value) {
    String normalized = value == null ? "" : value.replace(":", "").replace(" ", "").toLowerCase(Locale.ROOT);
    if (!normalized.matches("[0-9a-f]{64}")) throw new IllegalArgumentException("Enter the repository signing fingerprint (64 hexadecimal characters)");
    return normalized;
  }

  public static String normalizeUrl(String value) {
    try {
      URI uri = URI.create(value == null ? "" : value.trim());
      if (!"https".equalsIgnoreCase(uri.getScheme()) || host(uri) == null || uri.getUserInfo() != null || uri.getQuery() != null || uri.getFragment() != null)
        throw new IllegalArgumentException("Repository URL must be an HTTPS URL without credentials, query, or fragment");
      String path = uri.getPath() == null ? "" : uri.getPath().replaceAll("/+", "/");
      while (path.endsWith("/")) path = path.substring(0, path.length() - 1);
      return "https://" + IDN.toASCII(host(uri)).toLowerCase(Locale.ROOT) + path;
    } catch (IllegalArgumentException error) { throw error; }
    catch (Exception error) { throw new IllegalArgumentException("Invalid repository URL", error); }
  }

  public static String displayHost(String value) {
    try { return IDN.toASCII(host(URI.create(value))).toLowerCase(Locale.ROOT); }
    catch (Exception error) { throw new IllegalArgumentException("Invalid repository URL", error); }
  }

  private static String host(URI uri) {
    if (uri.getHost() != null) return uri.getHost();
    String authority = uri.getRawAuthority();
    if (authority == null || authority.contains("@")) return null;
    int port = authority.lastIndexOf(':');
    return port > 0 && authority.indexOf(':') == port ? authority.substring(0, port) : authority;
  }

  public static List<Map<String, Object>> visibleApps(List<Map<String, Object>> apps, Set<String> enabledRepositories) {
    List<Map<String, Object>> visible = new ArrayList<>();
    for (Map<String, Object> app : apps) if (enabledRepositories.contains(app.get("repository"))) visible.add(app);
    return visible;
  }
}
