// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "util/logging.hpp"
#include <iostream>
#include <map>
#include <mutex>
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

namespace unicity {
namespace util {

// Thread-safe initialization using std::call_once
static std::once_flag s_init_flag;
static std::once_flag s_shutdown_flag;

// Mutex protecting s_loggers map access (all reads and writes)
static std::mutex s_loggers_mutex;
static std::map<std::string, std::shared_ptr<spdlog::logger>> s_loggers;

// Internal initialization function (called via std::call_once)
static void InitializeInternal(const std::string &log_level, bool log_to_file,
                               const std::string &log_file_path) {
  try {
    // Create sinks
    std::vector<spdlog::sink_ptr> sinks;

    if (log_to_file) {
      namespace fs = std::filesystem;
      try {
        fs::path p = log_file_path.empty() ? fs::path("debug.log") : fs::path(log_file_path);
        if (p.has_parent_path() && !p.parent_path().empty()) {
          try { fs::create_directories(p.parent_path()); } catch (...) {/* best-effort */}
        }
        // Rotating file sink (max 10MB per file, 3 files total = 30MB max)
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            p.string(),
            10 * 1024 * 1024,  // 10MB max file size
            3);                // Keep 3 rotated files
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        sinks.push_back(file_sink);
      } catch (const spdlog::spdlog_ex &ex) {
        std::cerr << "Failed to initialize file logger (" << ex.what() << ") — falling back to console logging\n";
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        sinks.push_back(console_sink);
        log_to_file = false; // skip file-only post-init behavior below
      }
    } else {
      // Console sink for tests (colorized stdout)
      auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
      sinks.push_back(console_sink);
    }

    // Create loggers for different components
    std::vector<std::string> components = {"default", "network", "sync",
                                           "chain",   "crypto",  "app"};

    // Lock mutex for writing to s_loggers
    std::lock_guard<std::mutex> lock(s_loggers_mutex);

    for (const auto &component : components) {
      auto logger = std::make_shared<spdlog::logger>(component, sinks.begin(),
                                                     sinks.end());
      logger->set_level(spdlog::level::from_str(log_level));

      // Flush policy (Bitcoin Core-like): flush every message to make tail -f responsive
      // Apply to both console and file sinks
      logger->flush_on(spdlog::level::trace);

      spdlog::register_logger(logger);
      s_loggers[component] = logger;
    }

    // Set default logger
    spdlog::set_default_logger(s_loggers["default"]);

    // Add visual separator for new session (only if logging is enabled)
    if (log_to_file && log_level != "off") {
      for (int i = 0; i < 10; ++i) {
        spdlog::default_logger()->info("");
      }
    }

    // Direct logger access (cannot use LOG_INFO macro - would deadlock on mutex)
    // Only log initialization message if logging is actually enabled
    if (log_level != "off") {
      s_loggers["default"]->info("Logging system initialized (level: {})", log_level);
    }
  } catch (const spdlog::spdlog_ex &ex) {
    std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    // Exception safety: if init fails, s_loggers remains empty
    // Next Initialize() call will retry via std::call_once reset pattern
  }
}

void LogManager::Initialize(const std::string &log_level, bool log_to_file,
                            const std::string &log_file_path) {
  // Thread-safe: std::call_once ensures InitializeInternal runs exactly once
  // Multiple concurrent calls are safe - only first caller executes
  std::call_once(s_init_flag, InitializeInternal, log_level, log_to_file, log_file_path);
}

void LogManager::Shutdown() {
  // Thread-safe: Lock mutex to prevent concurrent access during shutdown
  std::lock_guard<std::mutex> lock(s_loggers_mutex);

  // Silently shutdown - "Shutdown complete" was already logged by application
  spdlog::shutdown();
  s_loggers.clear();

  // Note: s_init_flag cannot be reset (std::call_once design)
  // Subsequent logging after Shutdown() will auto-reinitialize via GetLogger()
}

std::shared_ptr<spdlog::logger> LogManager::GetLogger(const std::string &name) {
  // Auto-initialize with defaults if not initialized
  // Thread-safe: std::call_once ensures this happens exactly once
  Initialize();

  // Thread-safe: Lock mutex for reading s_loggers map
  std::lock_guard<std::mutex> lock(s_loggers_mutex);

  auto it = s_loggers.find(name);
  if (it != s_loggers.end()) {
    return it->second;
  }

  // Return default logger if component not found
  // If loggers are empty (e.g., prior init failed), install a minimal console logger.
  if (s_loggers.empty()) {
    try {
      auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
      auto logger = std::make_shared<spdlog::logger>("default", console_sink);
      logger->set_level(spdlog::level::off);  // Default to off for tests
      logger->flush_on(spdlog::level::trace);
      spdlog::register_logger(logger);
      s_loggers["default"] = logger;
      spdlog::set_default_logger(logger);
      return logger;
    } catch (...) {
      return nullptr;
    }
  }

  return s_loggers["default"];
}

void LogManager::SetLogLevel(const std::string &level) {
  // Thread-safe: Lock mutex for reading/writing s_loggers map
  std::lock_guard<std::mutex> lock(s_loggers_mutex);

  if (s_loggers.empty()) {
    // Not initialized yet, ignore
    return;
  }

  auto log_level = spdlog::level::from_str(level);
  for (auto &[name, logger] : s_loggers) {
    logger->set_level(log_level);
  }

  // Note: This LOG_INFO call will deadlock if it tries to GetLogger()
  // because we're holding s_loggers_mutex. But since we're already initialized,
  // the logger exists and we can use it directly without GetLogger().
  // Only log if changing to non-off level (avoid spam during tests)
  if (s_loggers.count("default") > 0 && level != "off") {
    s_loggers["default"]->info("Log level changed to: {}", level);
  }
}

void LogManager::SetComponentLevel(const std::string &component, const std::string &level) {
  // Thread-safe: Lock mutex for reading/writing s_loggers map
  std::lock_guard<std::mutex> lock(s_loggers_mutex);

  if (s_loggers.empty()) {
    // Not initialized yet, ignore
    return;
  }

  auto it = s_loggers.find(component);
  if (it != s_loggers.end()) {
    auto log_level = spdlog::level::from_str(level);
    it->second->set_level(log_level);

    // Direct logger access (avoid deadlock from GetLogger() calling Initialize())
    // Only log if changing to non-off level (avoid spam during tests)
    if (s_loggers.count("default") > 0 && level != "off") {
      s_loggers["default"]->info("Component '{}' log level set to: {}", component, level);
    }
  } else {
    // Direct logger access (avoid deadlock)
    // Only warn if logging is actually enabled
    if (s_loggers.count("default") > 0 && s_loggers["default"]->level() != spdlog::level::off) {
      s_loggers["default"]->warn("Unknown log component: {}", component);
    }
  }
}

} // namespace util
} // namespace unicity
