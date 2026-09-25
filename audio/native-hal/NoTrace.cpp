// ATRACE is intentionally disabled in this standalone HAL. This avoids the
// platform-private libcutils tracing ABI; stream processing does not depend
// on trace events.
#include <cstdint>

extern "C" uint64_t atrace_get_enabled_tags() { return 0; }
extern "C" void atrace_begin_body(const char*) {}
extern "C" void atrace_end_body() {}
