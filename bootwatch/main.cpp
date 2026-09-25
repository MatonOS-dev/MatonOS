#include "logic.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <cstdio>
#include <string>

static std::string property(const char* key) {
  char value[PROP_VALUE_MAX] = {};
  __system_property_get(key, value);
  return value;
}
static std::string log_snapshot() {
  std::string out;
  FILE* p = popen("/system/bin/logcat -d -t 600 -v brief 2>/dev/null", "r");
  if (!p) return out;
  char line[768];
  while (fgets(line, sizeof(line), p) && out.size() < 256 * 1024) out += line;
  pclose(p);
  return out;
}
int main() {
  __android_log_print(ANDROID_LOG_INFO, "matonos-bootwatch", "watcher started (debug entry)");
  for (unsigned int tick = 0; ; ++tick) {
    const bool done = property("sys.boot_completed") == "1";
    auto d = bootwatch::diagnose(log_snapshot(), done);
    if (!done && tick >= 24 && d.failure.empty()) {
      d.state = "failure";
      d.failure = "boot_deadline_exceeded";
      d.culprit = "unknown";
      d.chain = "sys.boot_completed unset after 120 seconds; no known signature matched";
      d.evidence = 1;
    }
    __android_log_print(ANDROID_LOG_INFO, "matonos-bootwatch",
      "state=%s failure=%s culprit=%s evidence=%d chain=%s",
      d.state.c_str(), d.failure.c_str(), d.culprit.c_str(), d.evidence, d.chain.c_str());
    if (done) return 0;
    sleep(tick < 24 ? 5 : 30);
  }
}
