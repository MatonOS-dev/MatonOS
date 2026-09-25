#pragma once
#include <string>

namespace bootwatch {
struct Diagnosis {
  std::string state;
  std::string failure;
  std::string culprit;
  std::string chain;
  int evidence = 0;
};
Diagnosis diagnose(const std::string& log, bool boot_completed);
}
