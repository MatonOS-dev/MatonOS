#include "logic.h"
#include <algorithm>

namespace bootwatch {
static bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}
Diagnosis diagnose(const std::string& log, bool boot_completed) {
  Diagnosis d;
  d.state = boot_completed ? "complete" : "watching";
  if (boot_completed) return d;

  const bool declared = has(log, "Found IModule/default in device VINTF manifest");
  const bool missing = has(log, "could not be found, trying to start it as a lazy AIDL service");
  const bool init_fail = has(log, "Could not find 'aidl/") && has(log, "IModule/default");
  const bool loop = has(log, "audioserver") && (has(log, "restarting") || has(log, "restart"));
  if (declared && missing && init_fail) {
    d.state = "failure";
    d.failure = "aidl_service_unavailable";
    d.culprit = "android.hardware.audio.core.IModule/default";
    d.chain = "audio IModule/default unregistered -> lazy service start failed";
    if (loop) d.chain += " -> audioserver restart loop";
    d.chain += " -> boot completion unobserved";
    d.evidence = 3 + (loop ? 1 : 0);
  } else if (has(log, "sys.boot_completed") && !boot_completed) {
    d.failure = "boot_incomplete_unclassified";
    d.culprit = "unknown";
    d.chain = "boot completion unset; no recognized dependency failure";
    d.evidence = 1;
  }
  return d;
}
}
