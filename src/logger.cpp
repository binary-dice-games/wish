// MIT License © 2025 Binary Dice Games
/// @file logger.cpp
/// @brief Per-session logging RMI service implementation.
#include <logger.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

// ── logger ────────────────────────────────────────────────────────────────────

logger::logger(bison::dynamic&& base, bool verbose, std::filesystem::path log_path)
    : dynamic(std::move(base)), verbose_(verbose), log_path_(std::move(log_path)) {
  if (!log_path_.empty()) {
    // Ensure parent directory exists.
    std::filesystem::create_directories(log_path_.parent_path());
    log_file_.open(log_path_, std::ios::out | std::ios::app);
    if (!log_file_) {
      // Non-fatal: log a warning to stderr and continue without file output.
      std::cerr << "[wish::logger] cannot open log file: " << log_path_ << "\n";
    }
  }
}

void logger::log(const std::string& level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(mtx_);
  write_locked(level, msg);
}

void logger::write_locked(const std::string& level, const std::string& msg) {
  // Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] message
  std::string line = "[" + timestamp() + "] [" + level + "] " + msg + "\n";
  if (verbose_) {
    std::cout << line << std::flush;
  }
  if (log_file_.is_open()) {
    log_file_ << line << std::flush;
  }
}

// ── registration ──────────────────────────────────────────────────────────────

void register_logger() {
  auto proto = dynamic_ptr{"__WishLogger"_key, {}};
  auto log_in = std::make_shared<dynamic>();
  log_in->addField("level"_key, field{std::string{}, attr<DisplayName>("level")});
  log_in->addField("msg"_key, field{std::string{}, attr<DisplayName>("msg")});
  proto->addMethod(
      "log"_key,
      bison::method{
          [](dynamic& s, const dynamic& p) -> dynamic {
            static_cast<logger&>(s).log(p.as<std::string>("level"_key), p.as<std::string>("msg"_key));
            return dynamic{};
          },
          dynamic_ptr{log_in},
          nullptr,
          attr<DisplayName>("log")});
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish
