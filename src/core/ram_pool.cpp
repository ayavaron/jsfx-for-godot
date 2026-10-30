#include "jsfx/ram_pool.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "jsfx/slider_update_queue.h"
#include "jsfx_host.h"

namespace jsfx::ram_pool {
namespace {

constexpr int kMaxBlocks = 64;
constexpr int kDefaultBlocks = 8;

// Lock-free unless this is the single-threaded Web build, where the audio
// callback runs on the main thread.
using BlockQueue = BasicBoundedQueue<void*, kSliderQueueLockFree>;

BlockQueue& pool() {
  static BlockQueue queue(kMaxBlocks);
  return queue;
}

std::atomic<int> g_target{0};
std::atomic<int> g_available{0};
std::atomic<std::uint64_t> g_misses{0};
thread_local bool t_realtime = false;

}  // namespace

void refill() {
  const int target = g_target.load(std::memory_order_relaxed);
  while (g_available.load(std::memory_order_acquire) < target) {
    void* block = std::calloc(1, kBlockBytes);
    if (block == nullptr) return;
    if (!pool().push(block)) {
      std::free(block);
      return;
    }
    g_available.fetch_add(1, std::memory_order_release);
  }
}

void release_all() {
  void* block = nullptr;
  while (pool().pop(block)) {
    g_available.fetch_sub(1, std::memory_order_acq_rel);
    std::free(block);
  }
}

void set_target_blocks(int blocks) {
  g_target.store(std::clamp(blocks, 0, kMaxBlocks), std::memory_order_relaxed);
}

int target_blocks() { return g_target.load(std::memory_order_relaxed); }

std::size_t available() {
  return static_cast<std::size_t>(
      std::max(0, g_available.load(std::memory_order_acquire)));
}

std::uint64_t misses() { return g_misses.load(std::memory_order_relaxed); }

void note_on_demand_vm() {
  int current = g_target.load(std::memory_order_relaxed);
  while (current < kDefaultBlocks &&
         !g_target.compare_exchange_weak(current, kDefaultBlocks,
                                         std::memory_order_relaxed)) {
  }
  refill();
}

void set_os_mapping_enabled(bool enabled) {
  jsfx_host_set_os_mapping_enabled(enabled ? 1 : 0);
}

bool os_mapping_enabled() { return jsfx_host_os_mapping_enabled() != 0; }

ScopedRealtime::ScopedRealtime() : previous_(t_realtime) { t_realtime = true; }

ScopedRealtime::~ScopedRealtime() { t_realtime = previous_; }

}  // namespace jsfx::ram_pool

/// Replaces calloc inside WDL's nseel-ram.c (see jsfx_ram_redirect.h).
extern "C" void* jsfx_ram_calloc(std::size_t count, std::size_t size) {
  using namespace jsfx::ram_pool;
  if (!t_realtime) return std::calloc(count, size);

  // The audio thread may only take ready-made blocks. Anything else (the
  // gmem tables, for example) fails, and EEL2 falls back to its failure slot.
  void* block = nullptr;
  if (count * size == kBlockBytes && pool().pop(block)) {
    g_available.fetch_sub(1, std::memory_order_acq_rel);
    return block;
  }
  g_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}
