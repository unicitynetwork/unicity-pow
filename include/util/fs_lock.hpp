// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace unicity {
namespace util {

namespace fs = std::filesystem;

/**
 * Cross-platform file lock
 * Uses fcntl() on Unix/macOS and LockFileEx() on Windows
 *
 * Simplified from Bitcoin Core's fsbridge::FileLock
 */
class FileLock {
public:
  FileLock() = delete;
  FileLock(const FileLock &) = delete;
  FileLock(FileLock &&) = delete;

  explicit FileLock(const fs::path &file);
  ~FileLock();

  /**
   * Try to acquire exclusive lock on file
   * @return true if lock acquired, false otherwise
   */
  bool TryLock();

  /**
   * Get reason for lock failure
   */
  const std::string& GetReason() const { return reason_; }

  // Platform-specific handles (public for error checking in LockDirectory)
  std::string reason_;
#ifndef _WIN32
  int fd_{-1};
#else
  void *hFile_{(void *)-1}; // INVALID_HANDLE_VALUE
#endif
};

/**
 * Result of directory lock attempt
 */
enum class LockResult {
  Success,    // Lock acquired successfully
  ErrorWrite, // Could not create lock file
  ErrorLock,  // Lock already held by another process
};

/**
 * Lock a directory to prevent multiple instances from using it
 *
 * Creates a .lock file in the directory and acquires an exclusive lock.
 * The lock is automatically released when the FileLock object is destroyed
 * or when UnlockDirectory is called.
 *
 * @param directory Directory to lock
 * @param lockfile_name Name of lock file (default: ".lock")
 * @param probe_only If true, only test if lock can be acquired (don't hold it)
 * @return LockResult indicating success or failure
 */
LockResult LockDirectory(const fs::path &directory,
                         const std::string &lockfile_name = ".lock",
                         bool probe_only = false);

/**
 * Release a directory lock
 *
 * @param directory Directory to unlock
 * @param lockfile_name Name of lock file (default: ".lock")
 */
void UnlockDirectory(const fs::path &directory,
                     const std::string &lockfile_name = ".lock");

/**
 * Release all directory locks
 * Used for cleanup during shutdown
 */
void ReleaseAllDirectoryLocks();

} // namespace util
} // namespace unicity


