#include "logic.h"
#include <cassert>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
  assert(argc == 2);
  std::ifstream f(argv[1]);
  std::stringstream buffer; buffer << f.rdbuf();
  const auto d = bootwatch::diagnose(buffer.str(), false);
  assert(d.state == "failure");
  assert(d.failure == "aidl_service_unavailable");
  assert(d.culprit == "android.hardware.audio.core.IModule/default");
  assert(d.evidence >= 3);
  assert(bootwatch::diagnose("", true).state == "complete");
  assert(bootwatch::diagnose("", false).failure.empty());
}
