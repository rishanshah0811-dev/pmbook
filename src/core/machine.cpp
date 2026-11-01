#include "core/machine.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <thread>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace pm {

std::string machine_description() {
  std::string cpu = "unknown CPU";
  long long mem_gb = 0;
#if defined(__APPLE__)
  char buf[256];
  std::size_t len = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) cpu.assign(buf);
  std::int64_t mem = 0;
  len = sizeof mem;
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) mem_gb = mem >> 30;
#else
  std::ifstream info("/proc/cpuinfo");
  for (std::string line; std::getline(info, line);) {
    if (line.rfind("model name", 0) == 0) {
      cpu = line.substr(line.find(':') + 2);
      break;
    }
  }
  mem_gb = static_cast<long long>(sysconf(_SC_PHYS_PAGES)) * sysconf(_SC_PAGE_SIZE) >> 30;
#endif
  utsname u{};
  uname(&u);
  return cpu + " | " + std::to_string(std::thread::hardware_concurrency()) + " cores | " +
         std::to_string(mem_gb) + " GB | " + u.sysname + " " + u.release + " " + u.machine;
}

std::string build_description() {
  std::string s;
#if defined(__clang__)
  s = "clang " __clang_version__;
#elif defined(__GNUC__)
  s = "gcc " __VERSION__;
#endif
#if defined(PM_BUILD_TYPE)
  s += std::string(" | ") + PM_BUILD_TYPE;
#endif
#if defined(PM_LTO)
  s += " | LTO on";
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  s += " | SANITIZED (do not quote)";
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  s += " | SANITIZED (do not quote)";
#endif
#endif
  return s;
}

}  // namespace pm
