#include "log.h"

namespace fl_log {
bool Init() {
#if SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_OFF
  return true;
#else
  try {
    auto existing = spdlog::get("file");
    if (existing) {
      spdlog::set_default_logger(existing);
    } else {
      auto logger = spdlog::basic_logger_mt("file", "FontLoaderSub.log", true);
      spdlog::set_default_logger(logger);
    }
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    spdlog::info(fmt::format("Logger initialized: {}", "FontLoaderSub.log"));
    return true;
  } catch (const spdlog::spdlog_ex &) {
    return false;
  }
#endif
}

void Shutdown() {
#if SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_OFF
  return;
#else
  auto logger = spdlog::get("file");
  if (logger) {
    logger->info("Logger shutdown");
  }
  spdlog::shutdown();
#endif
}
}  // namespace fl_log
