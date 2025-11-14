#include "util/files.hpp"
#include <cstdlib>
#include <fstream>
#include <random>

// Platform-specific includes for fsync
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif

namespace unicity {
namespace util {

namespace {

// fsync wrapper that works cross-platform
bool sync_file(int fd) {
#if defined(__APPLE__) || defined(__linux__)
  return fsync(fd) == 0;
#elif defined(_WIN32)
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  return FlushFileBuffers(h) != 0;
#else
  // Fallback: no-op (not safe but at least compiles)
  return true;
#endif
}

// Sync directory to ensure rename is durable
bool sync_directory(const std::filesystem::path &dir) {
#if defined(__APPLE__)
  // macOS doesn't have O_DIRECTORY flag
  int fd = open(dir.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  bool result = fsync(fd) == 0;
  close(fd);
  return result;
#elif defined(__linux__)
  int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return false;
  bool result = fsync(fd) == 0;
  close(fd);
  return result;
#elif defined(_WIN32)
  // Windows doesn't need explicit directory sync
  return true;
#else
  return true;
#endif
}

// Generate random suffix for temp file
// Uses thread_local static to avoid expensive RNG recreation
std::string random_suffix() {
  static thread_local std::mt19937 gen(std::random_device{}());
  static thread_local std::uniform_int_distribution<> dis(0, 0xFFFF);
  char buf[8];
  snprintf(buf, sizeof(buf), "%04x", dis(gen));
  return std::string(buf);
}

} // anonymous namespace

bool atomic_write_file(const std::filesystem::path &path,
                       const std::vector<uint8_t> &data,
                       int mode) {
  // Create parent directory if needed
  auto parent = path.parent_path();
  if (!parent.empty() && !ensure_directory(parent)) {
    return false;
  }

  // Generate temp file path
  auto temp_path = path;
  temp_path += ".tmp." + random_suffix();

#if defined(__APPLE__) || defined(__linux__)
  // Use POSIX file operations for full control over fsync
  int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) {
    return false;
  }

  // Write data (handle partial writes)
  size_t total = 0;
  while (total < data.size()) {
    ssize_t n = write(fd, data.data() + total, data.size() - total);
    if (n <= 0) {
      close(fd);
      std::filesystem::remove(temp_path);
      return false;
    }
    total += static_cast<size_t>(n);
  }

  // Sync to disk
  if (!sync_file(fd)) {
    close(fd);
    std::filesystem::remove(temp_path);
    return false;
  }

  close(fd);

#elif defined(_WIN32)
  // Windows: use std::ofstream with explicit flush
  std::ofstream temp_file(temp_path, std::ios::binary | std::ios::trunc);
  if (!temp_file) {
    return false;
  }

  // Write data
  temp_file.write(reinterpret_cast<const char *>(data.data()), data.size());
  if (!temp_file) {
    temp_file.close();
    std::filesystem::remove(temp_path);
    return false;
  }

  // Flush to OS buffers
  temp_file.flush();

  // Get handle and sync
  HANDLE h = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
    CloseHandle(h);
  }

  temp_file.close();

#else
  // Fallback: use std::ofstream without sync (not crash-safe)
  std::ofstream temp_file(temp_path, std::ios::binary | std::ios::trunc);
  if (!temp_file) {
    return false;
  }

  temp_file.write(reinterpret_cast<const char *>(data.data()), data.size());
  if (!temp_file) {
    temp_file.close();
    std::filesystem::remove(temp_path);
    return false;
  }

  temp_file.flush();
  temp_file.close();
#endif

  // Sync directory to ensure rename will be durable
  if (!parent.empty() && !sync_directory(parent)) {
    // Directory sync failed - clean up temp file
    std::filesystem::remove(temp_path);
    return false;
  }

  // Atomic rename
  std::error_code ec;
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    std::filesystem::remove(temp_path);
    return false;
  }

  return true;
}

bool atomic_write_file(const std::filesystem::path &path,
                       const std::vector<uint8_t> &data) {
  return atomic_write_file(path, data, 0644);
}

bool atomic_write_file(const std::filesystem::path &path,
                       const std::string &data,
                       int mode) {
  std::vector<uint8_t> vec(data.begin(), data.end());
  return atomic_write_file(path, vec, mode);
}

bool atomic_write_file(const std::filesystem::path &path,
                       const std::string &data) {
  return atomic_write_file(path, data, 0644);
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return {};
  }

  // Get file size with proper error checking
  std::streampos pos = file.tellg();
  if (pos == std::streampos(-1)) {
    return {};
  }

  std::streamsize size = static_cast<std::streamsize>(pos);
  if (size < 0) {
    return {};
  }

  // Sanity check: refuse to read files larger than 100MB
  // Prevents accidental memory exhaustion
  constexpr std::streamsize MAX_FILE_SIZE = 100 * 1024 * 1024;
  if (size > MAX_FILE_SIZE) {
    return {};
  }

  std::vector<uint8_t> data(size);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(data.data()), size);

  if (!file) {
    return {};
  }

  return data;
}

std::string read_file_string(const std::filesystem::path &path) {
  auto data = read_file(path);
  return std::string(data.begin(), data.end());
}

bool ensure_directory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec || std::filesystem::exists(dir);
}

std::filesystem::path get_default_datadir() {
  const char *home = nullptr;

#ifdef _WIN32
  home = std::getenv("APPDATA");
  if (home) {
    return std::filesystem::path(home) / "unicity";
  }
#else
  home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / ".unicity";
  }
#endif

  // Fallback to current directory
  return std::filesystem::current_path() / ".unicity";
}

} // namespace util
} // namespace unicity
