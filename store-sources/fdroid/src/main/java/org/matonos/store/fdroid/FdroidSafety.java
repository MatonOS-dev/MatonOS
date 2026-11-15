package org.matonos.store.fdroid;
import java.util.*;
final class FdroidSafety {
  private static final Map<String, String[]> FEATURES = new HashMap<>();
  static {
    FEATURES.put("Ads", new String[]{"Advertising", "This app contains advertising", "warning"});
    FEATURES.put("Tracking", new String[]{"Tracks you", "This app tracks and reports your activity", "danger"});
    FEATURES.put("NonFreeNet", new String[]{"Non-free network services", "This app promotes or depends on non-free network services", "warning"});
    FEATURES.put("TetheredNet", new String[]{"Tied to one service", "This app depends entirely on a certain instance of a network service", "warning"});
    FEATURES.put("NonFreeAdd", new String[]{"Promotes non-free add-ons", "This app promotes non-free add-ons", "warning"});
    FEATURES.put("NonFreeDep", new String[]{"Needs non-free apps", "This app depends on other non-free apps", "warning"});
    FEATURES.put("UpstreamNonFree", new String[]{"Upstream is non-free", "The upstream source code is not entirely free", "warning"});
    FEATURES.put("NonFreeAssets", new String[]{"Non-free assets", "This app contains non-free assets", "neutral"});
    FEATURES.put("KnownVuln", new String[]{"Known vulnerability", "This app contains a known security vulnerability", "danger"});
    FEATURES.put("DisabledAlgorithm", new String[]{"Insecure signature", "This app has a weak security signature", "danger"});
    FEATURES.put("NoSourceSince", new String[]{"Source no longer available", "The source code is no longer available", "warning"});
    FEATURES.put("NSFW", new String[]{"Adult content", "This app contains content that may not be suitable for all audiences", "neutral"});
  }
  static void apply(Map<String, Object> app) {
    List<Map<String, String>> anti = new ArrayList<>(), items = new ArrayList<>();
    boolean auditable = true; String level = "safe";
    Object raw = app.remove("rawAntiFeatures");
    if (raw instanceof List) for (Object value : (List<?>) raw) {
      Map<?, ?> entry = (Map<?, ?>) value; String id = String.valueOf(entry.get("id")), reason = String.valueOf(entry.get("reason"));
      String[] feature = FEATURES.getOrDefault(id, new String[]{id, "", "warning"});
      anti.add(record("id", id, "title", feature[0], "description", feature[1], "reason", reason));
      items.add(record("id", id, "title", feature[0], "description", reason.isEmpty() ? feature[1] : reason, "severity", feature[2]));
      if (feature[2].equals("danger")) level = "potentially-unsafe";
      else if (feature[2].equals("warning") && level.equals("safe")) level = "probably-safe";
      if (id.equals("UpstreamNonFree") || id.equals("NoSourceSince")) auditable = false;
    }
    if (auditable) items.add(record("id", "auditable", "title", "Auditable code", "description", "Built by F-Droid from public source code", "severity", "good"));
    Map<String, Object> safety = new LinkedHashMap<>(); safety.put("level", level); safety.put("items", items);
    app.put("antiFeatures", anti); app.put("safety", safety);
  }
  private static Map<String, String> record(String... pairs) { Map<String, String> r = new LinkedHashMap<>(); for (int i=0;i<pairs.length;i+=2) r.put(pairs[i], pairs[i+1]); return r; }
}
