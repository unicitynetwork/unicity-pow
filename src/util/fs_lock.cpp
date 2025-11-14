// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "util/fs_lock.hpp"
#include "util/logging.hpp"
#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace unicity {
namespace util {

// Global mutex to protect dir_locks map
static std::mutex g_dir_locks_mutex;

// Map of currently held directory locks
// Key: full path to lock file
// Value: FileLock object
static std::map<std::string, std::unique_ptr<FileLock>> g_dir_locks;

// ============================================================================
// FileLock implementation
// ============================================================================

#ifndef _WIN32
// Unix/macOS implementation using fcntl

static std::string GetErrorReason() { return std::strerror(errno); }

FileLock::FileLock(const fs::path &file) {
  // O_CREAT: Create file if it doesn't exist (fixes race condition)
  // O_CLOEXEC: Don't leak fd to child processes (prevents lock inheritance)
  // 0644: rw-r--r-- permissions
  fd_ = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ == -1) {
    reason_ = GetErrorReason();
  }
}

FileLock::~FileLock() {
  if (fd_ != -1) {
    // Closing the fd automatically releases the fcntl lock
    close(fd_);
  }
}

bool FileLock::TryLock() {
  if (fd_ == -1) {
    return false;
  }

  struct flock lock;
  lock.l_type = F_WRLCK; // Exclusive write lock
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0; // Lock entire file

  if (fcntl(fd_, F_SETLK, &lock) == -1) {
    reason_ = GetErrorReason();
    return false;
  }

  return true;
}

#else
// Windows implementation using LockFileEx

static std::string GetErrorReason() {
  DWORD error = GetLastError();
  char *message = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 (LPSTR)&message, 0, nullptr);
  // False positive: FormatMessageA modifies 'message' via out-parameter
  // cppcheck-suppress knownConditionTrueFalse
  std::string result(message ? message : "Unknown error");
  // cppcheck-suppress knownConditionTrueFalse
  if (message) {
    LocalFree(message);
  }
  return result;
}

FileLock::FileLock(const fs::path &file) {
  // OPEN_ALWAYS: Create file if it doesn't exist (fixes race condition)
  // FILE_SHARE_* flags allow other processes to read/delete while we hold lock
  hFile_ = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (hFile_ == INVALID_HANDLE_VALUE) {
    reason_ = GetErrorReason();
  }
}

FileLock::~FileLock() {
  if (hFile_ != INVALID_HANDLE_VALUE) {
    // Closing the handle automatically releases the lock
    CloseHandle(hFile_);
  }
}

bool FileLock::TryLock() {
  if (hFile_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  OVERLAPPED overlapped = {};
  if (!LockFileEx(hFile_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                  0, MAXDWORD, MAXDWORD, &overlapped)) {
    reason_ = GetErrorReason();
    return false;
  }

  return true;
}

#endif

// ============================================================================
// Directory locking functions
// ============================================================================

LockResult LockDirectory(const fs::path &directory,
                         const std::string &lockfile_name, bool probe_only) {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);

  fs::path lockfile_path = directory / lockfile_name;
  std::string lockfile_str = lockfile_path.string();

  // Check if we already have a lock on this directory
  if (g_dir_locks.find(lockfile_str) != g_dir_locks.end()) {
    return LockResult::Success;
  }

  // Create and lock the file atomically
  // FileLock constructor now uses O_CREAT/OPEN_ALWAYS, so no separate
  // creation step needed (fixes TOCTOU race condition)
  auto file_lock = std::make_unique<FileLock>(lockfile_path);

  // Check if file was opened successfully
  #ifndef _WIN32
  if (file_lock->fd_ == -1) {
    if (!probe_only) {
      LOG_CHAIN_ERROR("Failed to open lock file {}: {}", lockfile_path.string(),
                file_lock->GetReason());
    }
    return LockResult::ErrorWrite;
  }
  #else
  if (file_lock->hFile_ == INVALID_HANDLE_VALUE) {
    if (!probe_only) {
      LOG_CHAIN_ERROR("Failed to open lock file {}: {}", lockfile_path.string(),
                file_lock->GetReason());
    }
    return LockResult::ErrorWrite;
  }
  #endif

  // Try to acquire lock
  if (!file_lock->TryLock()) {
    // Only log errors in non-probe mode (fixes log pollution)
    if (!probe_only) {
      LOG_CHAIN_ERROR("Failed to lock directory {}: {}", directory.string(),
                file_lock->GetReason());
    }
    return LockResult::ErrorLock;
  }

  // Lock acquired successfully
  if (probe_only) {
    // NOTE: probe_only has inherent race condition
    // The lock will be released when file_lock destructs at end of function
    // Another process could acquire it before the caller acts on the result
    // This is unavoidable with fcntl/LockFileEx semantics
    LOG_CHAIN_TRACE("Probe successful for directory lock: {} (lock will be released)",
              directory.string());
    return LockResult::Success;
  } else {
    // Normal mode: store the lock to keep it held
    g_dir_locks.emplace(lockfile_str, std::move(file_lock));
    LOG_CHAIN_TRACE("Acquired directory lock: {}", directory.string());
    return LockResult::Success;
  }
}

void UnlockDirectory(const fs::path &directory,
                     const std::string &lockfile_name) {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);

  fs::path lockfile_path = directory / lockfile_name;
  std::string lockfile_str = lockfile_path.string();

  auto it = g_dir_locks.find(lockfile_str);
  if (it != g_dir_locks.end()) {
    LOG_CHAIN_TRACE("Released directory lock: {}", directory.string());
    g_dir_locks.erase(it);
  }
}

void ReleaseAllDirectoryLocks() {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);
  LOG_CHAIN_TRACE("Releasing all directory locks ({} locks)", g_dir_locks.size());
  g_dir_locks.clear();
}

} // namespace util
} // namespace unicity
