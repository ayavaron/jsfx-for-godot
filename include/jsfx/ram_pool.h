#pragma once

#include <cstddef>
#include <cstdint>

namespace jsfx {

/// EEL2 memory for platforms without lazily backed OS pages (Web).
///
/// On desktop each VM's 8M-slot window is one OS region whose pages become
/// resident on first touch. WebAssembly has no such pages, so there EEL2
/// allocates its 512 KiB blocks on demand instead. Allocation on the audio
/// thread is forbidden, so while `JsfxProcessor::process` runs, block requests
/// are served from this pool of pre-zeroed blocks, which the main thread tops
/// up (`refill`) every frame. If the pool is empty the access gets EEL2's
/// failure slot (reads 0, writes dropped) until the next refill, and `misses`
/// counts it. Allocation outside `process` (compile, `@init`) uses calloc.
namespace ram_pool {

/// Size of one EEL2 memory block (65536 slots of 8 bytes).
inline constexpr std::size_t kBlockBytes = 65536 * 8;

/// Main thread: allocate blocks until `target_blocks()` are available.
void refill();

/// Main thread: free every pooled block (shutdown, tests).
void release_all();

/// Free blocks kept in reserve. Defaults to 8 (4 MiB) once a VM uses the
/// pool, 0 before that; capped at 64.
void set_target_blocks(int blocks);
int target_blocks();

/// Blocks currently in the pool.
std::size_t available();

/// Audio-thread block requests that found the pool empty, since startup.
std::uint64_t misses();

/// Called by the EEL2 wrapper when a VM falls back to on-demand blocks.
void note_on_demand_vm();

/// Testing/benchmarking: disable desktop OS-backed mapping so desktop builds
/// exercise the Web allocation path. Affects VMs created afterwards.
void set_os_mapping_enabled(bool enabled);
bool os_mapping_enabled();

/// Marks the current thread as real-time while in scope: EEL2 block requests
/// go to the pool instead of calloc.
class ScopedRealtime {
 public:
  ScopedRealtime();
  ~ScopedRealtime();
  ScopedRealtime(const ScopedRealtime&) = delete;
  ScopedRealtime& operator=(const ScopedRealtime&) = delete;

 private:
  bool previous_;
};

}  // namespace ram_pool
}  // namespace jsfx
