// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "chain/randomx_pow.hpp"
#include "util/arith_uint256.hpp"
#include "util/sha256.hpp"
#include "util/logging.hpp"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace unicity {
namespace crypto {

// RandomX epoch seed string 
// - Same RandomX cache keys per epoch as Unicity Alpha network
// - Saves creaating a separate RandomX fork for Unicity POW

static const char *RANDOMX_EPOCH_SEED_STRING = "Alpha/RandomX/Epoch/%d";

// Mutex for cache access
static std::mutex g_randomx_mutex;

// RAII wrappers for RandomX objects
struct RandomXCacheWrapper {
  randomx_cache *cache = nullptr;

  explicit RandomXCacheWrapper(randomx_cache *c) : cache(c) {}
  ~RandomXCacheWrapper() {
    if (cache)
      randomx_release_cache(cache);
  }
};

// Simple LRU cache for RandomX VMs and caches (per-thread, bounded)
// Keeps only the most recent N epochs to prevent unbounded memory growth
template <typename Key, typename Value>
class SimpleLRUCache {
private:
  struct Entry {
    Key key;
    Value value;
  };
  
  std::vector<Entry> entries_;
  size_t max_size_;
  
public:
  explicit SimpleLRUCache(size_t max_size) : max_size_(max_size) {}
  
  // Get value if exists, returns nullptr if not found
  Value* get(const Key& key) {
    // Search from end (most recent) to beginning
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      if (it->key == key) {
        // Move to end (most recent)
        Entry entry = *it;
        entries_.erase(std::prev(it.base()));
        entries_.push_back(entry);
        return &entries_.back().value;
      }
    }
    return nullptr;
  }
  
  // Insert or update a value
  void insert(const Key& key, const Value& value) {
    // Remove if already exists
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key == key) {
        entries_.erase(it);
        break;
      }
    }
    
    // If at capacity, remove oldest (first) entry
    if (entries_.size() >= max_size_) {
      entries_.erase(entries_.begin());
    }
    
    // Add to end (most recent)
    entries_.push_back({key, value});
  }
  
  void clear() {
    entries_.clear();
  }
  
  size_t size() const {
    return entries_.size();
  }
};

// Thread-local cache storage: LRU with bounded size
static thread_local SimpleLRUCache<uint32_t, std::shared_ptr<RandomXCacheWrapper>>
    t_cache_storage(DEFAULT_RANDOMX_VM_CACHE_SIZE);

// Thread-local VM storage: LRU with bounded size
static thread_local SimpleLRUCache<uint32_t, std::shared_ptr<RandomXVMWrapper>>
    t_vm_cache(DEFAULT_RANDOMX_VM_CACHE_SIZE);

static bool g_randomx_initialized = false;

uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration) {
  return nTime / nDuration;
}

uint256 GetSeedHash(uint32_t nEpoch) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), RANDOMX_EPOCH_SEED_STRING, nEpoch);
  std::string s(buffer);

  uint256 h1, h2;
  CSHA256()
      .Write((const unsigned char *)s.data(), s.size())
      .Finalize(h1.begin());
  CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
  return h2;
}

// Get or create thread-local VM for an epoch
// Each thread gets its own VM instance for thread safety
// LRU eviction ensures bounded memory usage
std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  // Check if this thread already has a VM for this epoch (LRU get)
  auto vmPtr = t_vm_cache.get(nEpoch);
  if (vmPtr) {
    return *vmPtr;
  }

  uint256 seedHash = GetSeedHash(nEpoch);
  randomx_flags flags = randomx_get_flags();
  // Disable JIT and use secure mode (interpreter only)
  // flags = static_cast<randomx_flags>(flags & ~RANDOMX_FLAG_JIT);
  // flags = static_cast<randomx_flags>(flags | RANDOMX_FLAG_SECURE);

  // Get or create thread-local cache (each thread has isolated cache)
  std::shared_ptr<RandomXCacheWrapper> myCache;
  auto cachePtr = t_cache_storage.get(nEpoch);
  if (cachePtr) {
    myCache = *cachePtr;
  } else {
    LOG_CRYPTO_INFO("Creating thread-local RandomX cache for epoch {} (this may take a moment)...", nEpoch);

    randomx_cache *pCache = randomx_alloc_cache(flags);
    if (!pCache) {
      throw std::runtime_error("Failed to allocate RandomX cache");
    }
    randomx_init_cache(pCache, seedHash.data(), seedHash.size());
    myCache = std::make_shared<RandomXCacheWrapper>(pCache);
    // LRU insert: automatically evicts oldest if at capacity
    t_cache_storage.insert(nEpoch, myCache);

    LOG_CRYPTO_INFO("Created thread-local RandomX cache for epoch {}", nEpoch);
  }

  // Create thread-local VM (no lock needed, each thread has its own cache and VM)
  LOG_CRYPTO_INFO("Creating thread-local RandomX VM for epoch {}...", nEpoch);

  randomx_vm *myVM = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!myVM) {
    throw std::runtime_error("Failed to create RandomX VM");
  }

  auto vmWrapper = std::make_shared<RandomXVMWrapper>(myVM, myCache);
  // LRU insert: automatically evicts oldest epoch if at capacity
  t_vm_cache.insert(nEpoch, vmWrapper);

  LOG_CRYPTO_INFO("Created thread-local RandomX VM for epoch {} (LRU cache size: {})", nEpoch, t_vm_cache.size());

  return vmWrapper;
}

uint256 GetRandomXCommitment(const CBlockHeader &block, uint256 *inHash) {
  uint256 rx_hash = inHash == nullptr ? block.hashRandomX : *inHash;

  // Create copy of header with hashRandomX set to null
  CBlockHeader rx_blockHeader(block);
  rx_blockHeader.hashRandomX.SetNull();

  // Calculate commitment
  char rx_cm[RANDOMX_HASH_SIZE];
  randomx_calculate_commitment(&rx_blockHeader, sizeof(rx_blockHeader),
                               rx_hash.data(), rx_cm);

  return uint256(std::vector<unsigned char>(rx_cm, rx_cm + sizeof(rx_cm)));
}

void InitRandomX() {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (g_randomx_initialized) {
    return;
  }

  g_randomx_initialized = true;

  LOG_CRYPTO_INFO("RandomX initialized with LRU-bounded thread-local caches and VMs "
                  "(cache size: {})", DEFAULT_RANDOMX_VM_CACHE_SIZE);
}

void ShutdownRandomX() {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (!g_randomx_initialized) {
    return;
  }

  g_randomx_initialized = false;

  // Explicitly clear thread-local caches and VMs to prevent stale state
  // when RandomX is re-initialized in the same thread (e.g., during tests)
  t_vm_cache.clear();
  t_cache_storage.clear();

  LOG_CRYPTO_INFO("RandomX shutdown complete (thread-local caches and VMs "
                  "cleaned up)");
}

std::shared_ptr<RandomXVMWrapper> CreateVMForEpoch(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  uint256 seedHash = GetSeedHash(nEpoch);
  randomx_flags flags = randomx_get_flags();

  // Get or create thread-local cache (each thread has isolated cache)
  std::shared_ptr<RandomXCacheWrapper> myCache;
  auto cachePtr = t_cache_storage.get(nEpoch);
  if (cachePtr) {
    myCache = *cachePtr;
  } else {
    randomx_cache *pCache = randomx_alloc_cache(flags);
    if (!pCache) {
      throw std::runtime_error("Failed to allocate RandomX cache");
    }
    randomx_init_cache(pCache, seedHash.data(), seedHash.size());
    myCache = std::make_shared<RandomXCacheWrapper>(pCache);
    // LRU insert: automatically evicts oldest if at capacity
    t_cache_storage.insert(nEpoch, myCache);

    LOG_CRYPTO_INFO("Created thread-local RandomX cache for epoch {} (for "
                    "parallel verification)",
                    nEpoch);
  }

  // Create a new VM from the thread-local cache
  randomx_vm *vm = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!vm) {
    throw std::runtime_error(
        "Failed to create RandomX VM for parallel verification");
  }

  // Wrap in RAII smart pointer for automatic cleanup
  return std::make_shared<RandomXVMWrapper>(vm, myCache);
}

} // namespace crypto
} // namespace unicity
