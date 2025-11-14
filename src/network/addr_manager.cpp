#include "network/addr_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "util/files.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace unicity {
namespace network {

// Constants (Bitcoin Core aligned)
// How old addresses can maximally be 
static constexpr uint32_t ADDRMAN_HORIZON_DAYS = 30;

// After how many failed attempts we give up on a new node
static constexpr uint32_t ADDRMAN_RETRIES = 3;

// How many successive failures over ADDRMAN_MIN_FAIL_DAYS before an address
// with prior success (last_success > 0) is considered "terrible"
// Used in: is_terrible() to filter out repeatedly failing previously-good addresses
static constexpr uint32_t ADDRMAN_MAX_FAILURES = 10;

// In at least this duration
static constexpr uint32_t ADDRMAN_MIN_FAIL_DAYS = 7;

// After this many consecutive failures, demote a TRIED address back to NEW table
// Used in: failed() to implement Bitcoin Core's tried→new demotion on persistent failures
// Note: This has the same value as ADDRMAN_MAX_FAILURES but serves a different purpose:
//   - ADDRMAN_MAX_FAILURES: marks address as "terrible" (for filtering)
//   - TRIED_DEMOTION_THRESHOLD: triggers table movement (tried→new)
static constexpr uint32_t TRIED_DEMOTION_THRESHOLD = 10;

// An address in the NEW table is considered "stale" if we haven't heard about it for this many days.
// Stale NEW entries are removed by cleanup_stale(); TRIED entries are retained even if old (they worked before).
static constexpr uint32_t STALE_AFTER_DAYS = 30;

static constexpr uint32_t SECONDS_PER_DAY = 86400; // Seconds in one day (utility for time math)

// Selection tuning constants:
// - SELECT_TRIED_BIAS_PERCENT: initial probability (0..100) to draw from TRIED vs NEW,
//   preferring known-good peers while still exploring NEW.
static constexpr int SELECT_TRIED_BIAS_PERCENT = 50;

// Probabilistic selection constants for GetChance():
// - GETCHANCE_RECENT_ATTEMPT_SEC: if address was tried within this window, reduce selection
//   probability to 1% (deprioritize but don't eliminate recently-tried addresses)
static constexpr uint32_t GETCHANCE_RECENT_ATTEMPT_SEC = 600;  // 10 minutes

// Time-based filtering constants for is_terrible():
// - TERRIBLE_GRACE_PERIOD_SEC: never mark an address as terrible if tried within this window
//   (gives addresses a brief grace period after connection attempts)
static constexpr uint32_t TERRIBLE_GRACE_PERIOD_SEC = 60;  // 1 minute

// - TERRIBLE_FUTURE_TIMESTAMP_SEC: reject addresses with timestamps this far in the future
//   (protects against "flying DeLorean" timestamp attacks)
static constexpr uint32_t TERRIBLE_FUTURE_TIMESTAMP_SEC = 600;  // 10 minutes

// AddrInfo implementation
bool AddrInfo::is_stale(uint32_t now) const {
  if (timestamp == 0 || timestamp > now) return false; // avoid underflow and treat future/zero as not stale
  return (now - timestamp) > (STALE_AFTER_DAYS * SECONDS_PER_DAY);
}

bool AddrInfo::is_terrible(uint32_t now) const {
  // Reference: Bitcoin Core src/addrman.cpp AddrInfo::IsTerrible()

  // Never remove an address tried in the last minute (grace period)
  if (last_try > 0 && now > last_try && (now - last_try) < TERRIBLE_GRACE_PERIOD_SEC) {
    return false;
  }

  // Reject addresses with timestamps more than 10 minutes in the future ("flying DeLorean")
  if (timestamp > now && (timestamp - now) > TERRIBLE_FUTURE_TIMESTAMP_SEC) {
    return true;
  }

  // NOT SEEN IN RECENT HISTORY: Remove addresses not seen in ADDRMAN_HORIZON 
  // This applies to BOTH tried and new addresses
  if (timestamp > 0 && now > timestamp) {
    uint32_t days_since_seen = (now - timestamp) / SECONDS_PER_DAY;
    if (days_since_seen > ADDRMAN_HORIZON_DAYS) {
      return true;
    }
  }

  // TRIED N TIMES AND NEVER A SUCCESS:
  // NEW addresses: terrible after ADDRMAN_RETRIES (3) failed attempts with no success
  if (last_success == 0 && attempts >= ADDRMAN_RETRIES) {
    return true;
  }

  // N SUCCESSIVE FAILURES IN THE LAST WEEK:
  // Applies to addresses that have succeeded before (last_success > 0)
  // terrible after ADDRMAN_MAX_FAILURES (10) attempts over ADDRMAN_MIN_FAIL_DAYS (7 days)
  if (last_success > 0 && now > last_success && attempts >= ADDRMAN_MAX_FAILURES) {
    uint32_t days_since_success = (now - last_success) / SECONDS_PER_DAY;
    if (days_since_success >= ADDRMAN_MIN_FAIL_DAYS) {
      return true;
    }
  }

  return false;
}

double AddrInfo::GetChance(uint32_t now) const {
  // Bitcoin Core parity: probabilistic address selection
  // Reference: Bitcoin Core src/addrman.cpp AddrInfo::GetChance()
  double fChance = 1.0;

  // Deprioritize very recent attempts (< 10 minutes)
  // Core uses: if (now - m_last_try < 10min) fChance *= 0.01;
  if (last_try > 0 && now > last_try && (now - last_try) < GETCHANCE_RECENT_ATTEMPT_SEC) {
    fChance *= 0.01;  // 1% chance if tried in last 10 minutes
  }

  // Deprioritize after each failed attempt: 0.66^attempts
  // Core caps at 8 attempts: pow(0.66, std::min(nAttempts, 8))
  // This gives: 1 fail=66%, 2=44%, 3=29%, 4=19%, 5=13%, 6=8%, 7=5%, 8+=3.6%
  fChance *= std::pow(0.66, std::min(attempts, 8));

  return fChance;
}

namespace {
// Helper: Normalize IPv4-compatible addresses to IPv4-mapped (Bitcoin Core parity)
// IPv4-compatible (deprecated): ::w.x.y.z (12 zeros, then 4-byte IPv4)
// IPv4-mapped (canonical):     ::ffff:w.x.y.z (10 zeros, 0xff 0xff, then 4-byte IPv4)
protocol::NetworkAddress NormalizeAddress(const protocol::NetworkAddress& addr) {
  protocol::NetworkAddress normalized = addr;

  // Check if it's IPv4-compatible format: first 12 bytes zero, last 4 non-zero
  bool is_ipv4_compatible =
      std::all_of(addr.ip.begin(), addr.ip.begin() + 12, [](uint8_t b) { return b == 0; }) &&
      !std::all_of(addr.ip.begin() + 12, addr.ip.end(), [](uint8_t b) { return b == 0; });

  if (is_ipv4_compatible) {
    // Convert to IPv4-mapped format
    std::fill(normalized.ip.begin(), normalized.ip.begin() + 10, 0);
    normalized.ip[10] = 0xff;
    normalized.ip[11] = 0xff;
    // Last 4 bytes (IPv4 address) remain unchanged
  }

  return normalized;
}

// Helper: Validate that an address is routable (not all-zero, not unspecified)
// Returns true if address is valid for storage/routing
bool IsRoutable(const protocol::NetworkAddress& addr) noexcept {
  // Check for zero port
  if (addr.port == 0) {
    return false;
  }

  // Check for all-zero IP (unspecified address ::/0.0.0.0)
  if (std::all_of(addr.ip.begin(), addr.ip.end(), [](uint8_t b) { return b == 0; })) {
    return false;
  }

  // Additional checks can be added here:
  // - Reject RFC 1918 private ranges if desired
  // - Reject loopback addresses (::1, 127.0.0.0/8)
  // - Reject documentation ranges (RFC 5737, RFC 3849)

  return true;
}

// Helper: Serialize AddrInfo to JSON
nlohmann::json SerializeAddrInfo(const AddrInfo& info) {
  using json = nlohmann::json;
  json addr;

  // Serialize IP address
  addr["ip"] = json::array();
  for (size_t i = 0; i < 16; ++i) {
    addr["ip"].push_back(info.address.ip[i]);
  }

  // Serialize other fields
  addr["port"] = info.address.port;
  addr["services"] = info.address.services;
  addr["timestamp"] = info.timestamp;
  addr["last_try"] = info.last_try;
  addr["last_count_attempt"] = info.last_count_attempt;
  addr["last_success"] = info.last_success;
  addr["attempts"] = info.attempts;

  return addr;
}

// Helper: Deserialize AddrInfo from JSON
// Returns true on success, false if validation fails
bool DeserializeAddrInfo(const nlohmann::json& addr_json, AddrInfo& info) {
  try {
    protocol::NetworkAddress addr;

    // Deserialize IP address
    if (addr_json["ip"].size() != 16) {
      return false;
    }
    for (size_t i = 0; i < 16; ++i) {
      addr.ip[i] = addr_json["ip"][i].get<uint8_t>();
    }

    // Deserialize other fields
    addr.port = addr_json["port"].get<uint16_t>();
    addr.services = addr_json["services"].get<uint64_t>();

    info.address = addr;
    info.timestamp = addr_json["timestamp"].get<uint32_t>();
    info.last_try = addr_json["last_try"].get<uint32_t>();
    info.last_count_attempt = addr_json.value("last_count_attempt", 0);  
    info.last_success = addr_json["last_success"].get<uint32_t>();
    info.attempts = addr_json["attempts"].get<int>();

    return true;
  } catch (...) {
    return false;
  }
}
} // anonymous namespace

// AddressManager implementation
AddressManager::AddressManager() : rng_(std::random_device{}()) {}

uint32_t AddressManager::now() const {
  return static_cast<uint32_t>(util::GetTime());
}

void AddressManager::rebuild_key_vectors() {
  // Performance optimization: Rebuild O(1) lookup vectors after map modifications
  // Called after: add, good (tried←new), failed (new←tried), cleanup, Load
  tried_keys_.clear();
  tried_keys_.reserve(tried_.size());
  for (const auto& [key, _] : tried_) {
    tried_keys_.push_back(key);
  }

  new_keys_.clear();
  new_keys_.reserve(new_.size());
  for (const auto& [key, _] : new_) {
    new_keys_.push_back(key);
  }
}

std::mt19937 AddressManager::make_request_rng() {
  // SECURITY: Per-request entropy prevents offline seed brute-force attacks
  // An attacker observing getaddr responses could brute-force a static seed,
  // then predict future address selections to enable eclipse attacks.
  //
  // Bitcoin Core pattern: mix base RNG state with time for each request
  // Reference: FastRandomContext in Bitcoin Core's random.cpp
  std::seed_seq seq{
    static_cast<uint32_t>(rng_()),
    static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())
  };
  return std::mt19937(seq);
}

bool AddressManager::add(const protocol::NetworkAddress &addr,
                         uint32_t timestamp) {
  std::lock_guard<std::mutex> lock(mutex_);
  return add_internal(addr, timestamp);
}

bool AddressManager::add_internal(const protocol::NetworkAddress &addr,
                                  uint32_t timestamp) {
  // Bitcoin Core parity: Normalize IPv4-compatible addresses to IPv4-mapped
  protocol::NetworkAddress normalized = NormalizeAddress(addr);

  // Validate address is routable
  if (!IsRoutable(normalized)) {
    return false;
  }

  const uint32_t now_s = now();
  // Clamp future or absurdly old timestamps to now
  const uint32_t TEN_YEARS = 10u * 365u * 24u * 60u * 60u;
  uint32_t eff_ts = (timestamp == 0 ? now_s : timestamp);
  if (eff_ts > now_s || now_s - eff_ts > TEN_YEARS) eff_ts = now_s;

  AddrInfo info(normalized, eff_ts);
  AddrKey key(normalized);

  // Check if already in tried table
  if (auto it = tried_.find(key); it != tried_.end()) {
    // Update timestamp if newer
    if (eff_ts > it->second.timestamp) {
      it->second.timestamp = eff_ts;
    }
    return false; // Already have it
  }

  // Check if already in new table
  if (auto it = new_.find(key); it != new_.end()) {
    // Update timestamp if newer
    if (eff_ts > it->second.timestamp) {
      it->second.timestamp = eff_ts;
    }
    return false; // Already have it
  }

  // Filter out terrible addresses
  if (info.is_terrible(now_s)) {
    return false;
  }

  // Add to new table
  new_[key] = info;
  new_keys_.push_back(key);  // O(1) append to key vector
  return true;
}

size_t AddressManager::add_multiple(
    const std::vector<protocol::TimestampedAddress> &addresses) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t added = 0;
  for (const auto &ts_addr : addresses) {
    if (add_internal(ts_addr.address, ts_addr.timestamp)) {
      added++;
    }
  }

  return added;
}

void AddressManager::attempt(const protocol::NetworkAddress &addr, bool fCountFailure) {
  std::lock_guard<std::mutex> lock(mutex_);

  protocol::NetworkAddress normalized = NormalizeAddress(addr);
  AddrKey key(normalized);
  uint32_t t = now();

  // Try new table first
  if (auto it = new_.find(key); it != new_.end()) {
    it->second.last_try = t;

    // Bitcoin Core parity: Only increment attempts if:
    // 1. fCountFailure is true (caller wants to count this as a failure)
    // 2. last_count_attempt < m_last_good (prevents double-counting)
    if (fCountFailure && it->second.last_count_attempt < m_last_good_) {
      it->second.last_count_attempt = t;
      it->second.attempts++;
    }
    return;
  }

  // Update tried table
  if (auto it = tried_.find(key); it != tried_.end()) {
    it->second.last_try = t;

    // Same logic for tried addresses
    if (fCountFailure && it->second.last_count_attempt < m_last_good_) {
      it->second.last_count_attempt = t;
      it->second.attempts++;
    }
  }
}

void AddressManager::good(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  protocol::NetworkAddress normalized = NormalizeAddress(addr);
  AddrKey key(normalized);
  uint32_t current_time = now();

  // Bitcoin Core parity: Update m_last_good timestamp
  // This is used in attempt() to prevent double-counting attempts
  m_last_good_ = current_time;

  // Check if in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    // Move from new to tried
    new_it->second.tried = true;
    new_it->second.last_success = current_time;
    new_it->second.attempts = 0; // Reset failure count

    tried_[key] = std::move(new_it->second);
    new_.erase(new_it);

    // Performance: Incremental vector updates (O(n) removal, O(1) append)
    new_keys_.erase(std::remove(new_keys_.begin(), new_keys_.end(), key), new_keys_.end());
    tried_keys_.push_back(key);

    LOG_NET_TRACE("Address moved from 'new' to 'tried'. New size: {}, Tried size: {}",
                  new_.size(), tried_.size());
    return;
  }

  // Already in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    tried_it->second.last_success = current_time;
    tried_it->second.attempts = 0; // Reset failure count
    return;
  }

  LOG_NET_WARN("AddressManager::good() called for unknown address");
}

void AddressManager::failed(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  protocol::NetworkAddress normalized = NormalizeAddress(addr);
  AddrKey key(normalized);

  // Update in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    new_it->second.attempts++;

    // Remove if too many failures
    if (new_it->second.is_terrible(now())) {
      new_.erase(new_it);
      // Performance: Incremental vector update (O(n) removal)
      new_keys_.erase(std::remove(new_keys_.begin(), new_keys_.end(), key), new_keys_.end());
    }
    return;
  }

  // Update in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    tried_it->second.attempts++;

    // Move back to new table if too many failures
    if (tried_it->second.attempts >= TRIED_DEMOTION_THRESHOLD) {
      tried_it->second.tried = false;
      new_[key] = std::move(tried_it->second);
      tried_.erase(tried_it);
      // Performance: Incremental vector updates (O(n) removal, O(1) append)
      tried_keys_.erase(std::remove(tried_keys_.begin(), tried_keys_.end(), key), tried_keys_.end());
      new_keys_.push_back(key);
    }
  }
}

std::optional<protocol::NetworkAddress> AddressManager::select() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (tried_.empty() && new_.empty()) {
    return std::nullopt;
  }

  // SECURITY: Use per-request RNG to prevent seed prediction attacks
  auto local_rng = make_request_rng();

  // Prefer tried addresses (SELECT_TRIED_BIAS_PERCENT% of the time)
  std::uniform_int_distribution<int> dist(0, 99);
  bool search_tried = !tried_.empty() && (dist(local_rng) < SELECT_TRIED_BIAS_PERCENT || new_.empty());

  const uint32_t now_ts = now();

  // Bitcoin Core parity: Escalating chance_factor for probabilistic selection
  // Reference: Bitcoin Core src/addrman.cpp Select_() 
  // Start with chance_factor = 1.0, multiply by 1.2 after each failed selection
  // This ensures we eventually select something even if all addresses have low GetChance()
  double chance_factor = 1.0;
  std::uniform_real_distribution<double> chance_dist(0.0, 1.0);


  // Escalating chance_factor, We'll use a reasonable iteration limit
  // to prevent infinite loops in edge cases
  const size_t max_iterations = 200;

  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    // Select table to search (alternates if chosen table is empty)
    bool use_tried = search_tried && !tried_.empty();
    if (!use_tried && new_.empty()) {
      use_tried = true;  // Fall back to tried if new is empty
    }

    // Pick random entry from selected table (O(1) using key vectors)
    if (use_tried) {
      std::uniform_int_distribution<size_t> idx_dist(0, tried_keys_.size() - 1);
      size_t idx = idx_dist(local_rng);
      const AddrKey& key = tried_keys_[idx];
      const AddrInfo& info = tried_.at(key);

      // Core uses: if (randbits(30) < chance_factor * GetChance() * (1<<30))
      // We use: if (rand(0,1) < chance_factor * GetChance())
      double effective_chance = std::min(1.0, chance_factor * info.GetChance(now_ts));
      if (chance_dist(local_rng) < effective_chance) {
        return info.address;
      }
    } else {
      std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
      size_t idx = idx_dist(local_rng);
      const AddrKey& key = new_keys_[idx];
      const AddrInfo& info = new_.at(key);

      double effective_chance = std::min(1.0, chance_factor * info.GetChance(now_ts));
      if (chance_dist(local_rng) < effective_chance) {
        return info.address;
      }
    }

    // Failed to select - increase chance_factor for next iteration
    chance_factor *= 1.2;

    // After 23 iterations: chance_factor ≈ 114, so even 0.01 addresses have >100% chance
  }

  // Absolute fallback (should never reach here with chance_factor escalation)
  // But included for safety in case of extreme edge cases
  if (!tried_.empty()) {
    auto it = tried_.begin();
    return it->second.address;
  }
  if (!new_.empty()) {
    auto it = new_.begin();
    return it->second.address;
  }

  return std::nullopt;
}

std::optional<protocol::NetworkAddress>
AddressManager::select_new_for_feeler() {
  std::lock_guard<std::mutex> lock(mutex_);

  // FEELER connections test addresses from "new" table (never connected before)
  // This helps move working addresses from "new" to "tried"
  if (new_.empty()) {
    return std::nullopt;
  }

  // SECURITY: Use per-request RNG to prevent seed prediction attacks
  auto local_rng = make_request_rng();

  // Bitcoin Core parity: Prefer addresses not tried in the last 10 minutes
  // This prevents wasting feeler connections on recently-probed peers
  static constexpr uint32_t FEELER_MIN_RETRY_SECONDS = 600; // 10 minutes
  const uint32_t now_ts = now();

  // Try up to 50 random selections to find an address not recently tried
  // If all addresses were recently tried, fall back to any address
  for (int attempts = 0; attempts < 50; ++attempts) {
    std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
    size_t idx = idx_dist(local_rng);
    const AddrKey& key = new_keys_[idx];
    const AddrInfo& info = new_.at(key);

    // Prefer addresses never tried or tried more than 10 minutes ago
    if (info.last_try == 0 || now_ts < info.last_try || (now_ts - info.last_try) >= FEELER_MIN_RETRY_SECONDS) {
      return info.address;
    }
  }

  // Fallback: all addresses were recently tried, return any address
  std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
  size_t idx = idx_dist(local_rng);
  const AddrKey& key = new_keys_[idx];
  return new_.at(key).address;
}

std::vector<protocol::TimestampedAddress>
AddressManager::get_addresses(size_t max_count) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<protocol::TimestampedAddress> result;
  result.reserve(std::min(max_count, tried_.size() + new_.size()));

  const uint32_t now_s = now();

  // Add tried addresses first (filter invalid/terrible defensively)
  for (const auto &[key, info] : tried_) {
    if (result.size() >= max_count) break;
    if (!IsRoutable(info.address)) continue;
    if (info.is_terrible(now_s)) continue;
    result.push_back({info.timestamp, info.address});
  }

  // Add new addresses (skip invalid/terrible)
  for (const auto &[key, info] : new_) {
    if (result.size() >= max_count) break;
    if (!IsRoutable(info.address)) continue;
    if (info.is_terrible(now_s)) continue;
    result.push_back({info.timestamp, info.address});
  }

  // SECURITY: Use per-request RNG to prevent seed prediction attacks
  // Shuffle for privacy (prevents enumeration of address table order)
  auto local_rng = make_request_rng();
  std::shuffle(result.begin(), result.end(), local_rng);

  return result;
}

size_t AddressManager::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tried_.size() + new_.size();
}

size_t AddressManager::tried_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tried_.size();
}

size_t AddressManager::new_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return new_.size();
}

void AddressManager::cleanup_stale() {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t current_time = now();

  size_t removed = 0;
  // Remove stale addresses from new table
  for (auto it = new_.begin(); it != new_.end();) {
    if (it->second.is_stale(current_time) ||
        it->second.is_terrible(current_time)) {
      it = new_.erase(it);
      removed++;
    } else {
      ++it;
    }
  }

  // Performance: Rebuild new_keys_ if any entries were removed
  if (removed > 0) {
    rebuild_key_vectors();
  }

  // Keep tried addresses even if old (they worked before)
}

bool AddressManager::Save(const std::string &filepath) {
  using json = nlohmann::json;

  std::lock_guard<std::mutex> lock(mutex_);

  try {
    // Calculate size without calling size() to avoid recursive lock
    size_t total_size = tried_.size() + new_.size();
    LOG_NET_TRACE("saving {} peer addresses to {}", total_size, filepath);

    json root;
    root["version"] = 1;
    root["tried_count"] = tried_.size();
    root["new_count"] = new_.size();
    root["m_last_good"] = m_last_good_;  

    // Save tried addresses
    json tried_array = json::array();
    for (const auto &[key, info] : tried_) {
      tried_array.push_back(SerializeAddrInfo(info));
    }
    root["tried"] = tried_array;

    // Save new addresses
    json new_array = json::array();
    for (const auto &[key, info] : new_) {
      new_array.push_back(SerializeAddrInfo(info));
    }
    root["new"] = new_array;

    // Atomic write: write to temp then rename (with fsync for durability)
    std::string data = root.dump(2);

    // Use centralized atomic write with 0600 permissions (owner-only)
    // Peer address data reveals network topology - restrict to owner only
    // This provides: temp file creation, partial write handling, fsync,
    // directory sync, and atomic rename - more robust than previous implementation
    if (!util::atomic_write_file(filepath, data, 0600)) {
      LOG_NET_ERROR("Failed to save addresses to {}", filepath);
      return false;
    }

    LOG_NET_TRACE("successfully saved {} addresses ({} tried, {} new)",
                  total_size, tried_.size(), new_.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during Save: {}", e.what());
    return false;
  }
}

bool AddressManager::Load(const std::string &filepath) {
  using json = nlohmann::json;

  std::lock_guard<std::mutex> lock(mutex_);

  try {
    LOG_NET_TRACE("loading peer addresses from {}", filepath);

    // Open file
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_TRACE("peer address file not found: {} (starting fresh)",
                   filepath);
      return false;
    }

    // Parse JSON
    json root;
    file >> root;
    file.close();

    // Validate version
    int version = root.value("version", 0);
    if (version != 1) {
      LOG_NET_ERROR("Unsupported peers file version: {}", version);
      return false;
    }

    // Rely on nlohmann::json parser error detection for corruption
    // (manual checksums over JSON text are fragile to whitespace/key-order changes)

    // Load m_last_good_ 
    m_last_good_ = root.value("m_last_good", 1);  // Default to 1 if not present

    // Clear existing data
    tried_.clear();
    new_.clear();

    // Load tried addresses
    if (root.contains("tried")) {
      for (const auto &addr_json : root["tried"]) {
        AddrInfo info;
        if (!DeserializeAddrInfo(addr_json, info)) {
          LOG_NET_TRACE("invalid address in tried table, skipping");
          continue;
        }
        info.tried = true;
        tried_[AddrKey(info.address)] = info;
      }
    }

    // Load new addresses
    if (root.contains("new")) {
      for (const auto &addr_json : root["new"]) {
        AddrInfo info;
        if (!DeserializeAddrInfo(addr_json, info)) {
          LOG_NET_WARN("invalid address in new table, skipping");
          continue;
        }
        info.tried = false;
        new_[AddrKey(info.address)] = info;
      }
    }

    // Performance: Rebuild key vectors after loading
    // Exception safety: If rebuild throws, clear everything to maintain invariants
    try {
      rebuild_key_vectors();
    } catch (const std::bad_alloc &e) {
      LOG_NET_ERROR("Failed to rebuild key vectors (out of memory): {}", e.what());
      tried_.clear();
      new_.clear();
      tried_keys_.clear();
      new_keys_.clear();
      throw; // Re-throw to outer catch
    }

    // Calculate total size without calling size() to avoid recursive lock
    size_t total_size = tried_.size() + new_.size();
    LOG_NET_INFO("Successfully loaded {} addresses ({} tried, {} new)",
                 total_size, tried_.size(), new_.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during Load: {}", e.what());
    tried_.clear();
    new_.clear();
    tried_keys_.clear();
    new_keys_.clear();
    return false;
  }
}

} // namespace network
} // namespace unicity
