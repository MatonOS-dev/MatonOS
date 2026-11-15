package org.matonos.store.fdroid;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import java.util.*;

/** Trusted index signers. Presets are seeded once; deleted presets stay deleted. */
public final class FdroidRepositories {
  public static final String OFFICIAL_URL = "https://f-droid.org/repo";
  private static final String[][] PRESETS = {
    {"https://apt.izzysoft.de/fdroid/repo", "3bf0d6abfeae2f401707b6d966be743bf0eee49c2561b9ba39073711f628937a"},
    {"https://microg.org/fdroid/repo", "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165"},
    {"https://guardianproject.info/fdroid/repo", "b7c2eefd8dac7806af67dfcd92eb18126bc08312a7f2d6f3862e46013c7a6135"},
    {"https://cdn.kde.org/android/stable-releases/fdroid/repo", "13784ba6c80ff4e2181e55c56f961eed5844cea16870d3b38d58780b85e1158f"},
    {"https://archive.newpipe.net/fdroid/repo", "e2402c78f9b97c6c89e97db914a2751fda1d02fe2039cc0897a462bdb57e7501"},
    {"https://www.collaboraoffice.com/downloads/fdroid/repo", "573258c84e149b5f4d9299e7434b2b69a8410372921d4ae586ba91ec767892cc"},
    {"https://mobileapp.bitwarden.com/fdroid/repo", "bc54ea6fd1cd5175bcccc47c561c5726e1c3ed7e686b6db4b18bac843a3efe6c"},
    {"https://molly.im/fdroid/foss/fdroid/repo", "5198daef37fc23c14d5ee32305b2af45787bd7df2034de33ad302bdb3446df74"},
    {"https://briarproject.org/fdroid/repo", "1fb874bee7276d28ecb2c9b06e8a122ec4bcb4008161436ce474c257cbf49bd6"},
    {"https://fdroid.getsession.org/fdroid/repo", "db0e5297eb65cc22d6bd93c869943bdcfcb6a07dc69a48a0dd8c7ba698ec04e6"},
    {"https://releases.threema.ch/fdroid/repo", "5734e753899b25775d90fe85362a49866e05ac4f83c05bef5a92880d2910639e"},
    {"https://static.cryptomator.org/android/fdroid/repo", "f7c3ec3b0d588d3cb52983e9eb1a7421c93d4339a286398e71d7b651e8d8ecdd"},
    {"https://freeyourgadget.codeberg.page/fdroid/repo", "cd381eccc465ab324e21bcc335895615e07e70ee11e9fd1df3c020c5194f00b2"},
    {"https://app.futo.org/fdroid/repo", "39d47869d29cbfce4691d9f7e6946a7b6d7e6ff4883497e6e675744ecdfa6d6d"},
    {"https://fdroid.ironfoxoss.org/fdroid/repo", "c5e291b5a571f9c8cd9a9799c2c94e02ec9703948893f2ca756d67b94204f904"},
    {"https://breezy-weather.github.io/fdroid-repo/fdroid/repo", "3480a7bb2a296d8f98cb90d2309199b5b9519c1b31978dbcd877adb102af35ee"},
    {"https://thunderbird.github.io/fdroid-thunderbird/repo", "8b86e5d48983f0875f7eb7a1b2f91b225ee5b997e463e3d63d0e2556e53666be"},
  };
  private final SharedPreferences prefs;
  public FdroidRepositories(Context context) { prefs = context.getSharedPreferences("fdroid-repositories", Context.MODE_PRIVATE); }
  private Set<String> values(String key) { return new LinkedHashSet<>(prefs.getStringSet(key, Collections.emptySet())); }
  public synchronized List<Map<String, Object>> list() {
    Set<String> trusted = values("trusted");
    if (!prefs.getBoolean("presets-seeded", false)) {
      for (String[] preset : PRESETS) if (trusted.stream().noneMatch(v -> v.startsWith(preset[0] + "\t"))) trusted.add(preset[0] + "\t" + preset[1]);
      save(prefs.edit().putStringSet("trusted", trusted).putBoolean("presets-seeded", true));
    }
    List<Map<String, Object>> result = new ArrayList<>();
    result.add(repo(OFFICIAL_URL, FdroidIndexClient.OFFICIAL_FINGERPRINT, true));
    for (String item : trusted) {
      String[] parts = item.split("\t", 2);
      if (parts.length != 2) throw new IllegalStateException("Malformed trusted repository: " + item);
      result.add(repo(parts[0], parts[1], false));
    }
    return result;
  }
  private Map<String, Object> repo(String url, String fingerprint, boolean official) {
    Map<String, Object> r = new LinkedHashMap<>();
    r.put("id", url); r.put("url", url); r.put("label", official ? "F-Droid (built in)" : FdroidRepository.displayHost(url));
    r.put("fingerprint", fingerprint); r.put("official", official); r.put("broken", values("broken").contains(url)); return r;
  }
  public synchronized String add(String shareUrl) {
    list();
    Uri uri = Uri.parse(shareUrl);
    String fingerprint = FdroidRepository.normalizeFingerprint(uri.getQueryParameter("fingerprint"));
    String url = FdroidRepository.normalizeUrl(uri.buildUpon().clearQuery().fragment(null).build().toString());
    if (OFFICIAL_URL.equals(url)) throw new IllegalArgumentException("The built-in F-Droid repository is already enabled");
    Set<String> trusted = values("trusted"); trusted.removeIf(v -> v.startsWith(url + "\t")); trusted.add(url + "\t" + fingerprint);
    Set<String> broken = values("broken"); broken.add(url);
    save(prefs.edit().putStringSet("trusted", trusted).putStringSet("broken", broken)); return url;
  }
  public synchronized void remove(String id) {
    list(); String url = FdroidRepository.normalizeUrl(id);
    if (OFFICIAL_URL.equals(url)) throw new IllegalArgumentException("The built-in F-Droid repository cannot be removed");
    Set<String> trusted = values("trusted"); trusted.removeIf(v -> v.startsWith(url + "\t"));
    Set<String> broken = values("broken"); broken.remove(url);
    save(prefs.edit().putStringSet("trusted", trusted).putStringSet("broken", broken));
  }
  public synchronized void setBroken(String url, boolean broken) {
    Set<String> values = values("broken"); if (broken) values.add(url); else values.remove(url);
    save(prefs.edit().putStringSet("broken", values));
  }
  private void save(SharedPreferences.Editor editor) { if (!editor.commit()) throw new IllegalStateException("Cannot persist trusted repositories"); }
}
