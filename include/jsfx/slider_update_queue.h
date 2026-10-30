#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace jsfx {

/// One parameter change travelling from a producer thread to the audio thread.
struct SliderUpdate {
  int index = 0;
  double value = 0.0;
};

inline constexpr std::size_t kDefaultSliderQueueCapacity = 1024;

/// Fixed-capacity bounded queue of trivially copyable values.
///
/// `BasicBoundedQueue<T, true>` is a lock-free ring buffer with Vyukov-style
/// per-cell sequence numbers; both ends use CAS, so it is safe for multiple
/// producers and multiple consumers. `BasicBoundedQueue<T, false>` is the PRD
/// Story 4.1 single-threaded Wasm fallback: plain indices, no atomics, because
/// the audio callback runs on the main thread and races are impossible.
///
/// Both variants allocate their storage in the constructor. `push` and `pop`
/// never allocate. `push` returns false when full; callers should retry or
/// coalesce rather than block.
template <typename T, bool LockFree>
class BasicBoundedQueue;

template <typename T>
class BasicBoundedQueue<T, true> {
 public:
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "The JSFX parameter queue requires lock-free size_t atomics");

  explicit BasicBoundedQueue(
      std::size_t capacity = kDefaultSliderQueueCapacity)
      : capacity_(round_up_capacity(capacity)),
        mask_(capacity_ - 1),
        cells_(new Cell[capacity_]) {
    for (std::size_t i = 0; i < capacity_; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~BasicBoundedQueue() = default;
  BasicBoundedQueue(const BasicBoundedQueue&) = delete;
  BasicBoundedQueue& operator=(const BasicBoundedQueue&) = delete;

  bool push(const T& update) noexcept {
    std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[position & mask_];
      const std::size_t sequence =
          cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t difference =
          static_cast<std::intptr_t>(sequence) -
          static_cast<std::intptr_t>(position);

      if (difference == 0) {
        if (enqueue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed)) {
          cell.data = update;
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;  // full
      } else {
        position = enqueue_position_.load(std::memory_order_relaxed);
      }
    }
  }

  bool pop(T& out) noexcept {
    std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[position & mask_];
      const std::size_t sequence =
          cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t difference =
          static_cast<std::intptr_t>(sequence) -
          static_cast<std::intptr_t>(position + 1);

      if (difference == 0) {
        if (dequeue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed)) {
          out = cell.data;
          cell.sequence.store(position + capacity_, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;  // empty
      } else {
        position = dequeue_position_.load(std::memory_order_relaxed);
      }
    }
  }

  std::size_t capacity() const noexcept { return capacity_; }
  static constexpr bool is_lock_free() noexcept { return true; }

 private:
  struct Cell {
    std::atomic<std::size_t> sequence{0};
    T data{};
  };

  static std::size_t round_up_capacity(std::size_t requested) {
    std::size_t capacity = 2;
    while (capacity < requested) capacity <<= 1;
    return capacity;
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  std::atomic<std::size_t> enqueue_position_{0};
  std::atomic<std::size_t> dequeue_position_{0};
};

template <typename T>
class BasicBoundedQueue<T, false> {
 public:
  explicit BasicBoundedQueue(
      std::size_t capacity = kDefaultSliderQueueCapacity)
      : capacity_(round_up_capacity(capacity)),
        mask_(capacity_ - 1),
        cells_(new T[capacity_]) {}

  ~BasicBoundedQueue() = default;
  BasicBoundedQueue(const BasicBoundedQueue&) = delete;
  BasicBoundedQueue& operator=(const BasicBoundedQueue&) = delete;

  bool push(const T& update) noexcept {
    if (tail_ - head_ >= capacity_) return false;
    cells_[tail_ & mask_] = update;
    ++tail_;
    return true;
  }

  bool pop(T& out) noexcept {
    if (head_ == tail_) return false;
    out = cells_[head_ & mask_];
    ++head_;
    return true;
  }

  std::size_t capacity() const noexcept { return capacity_; }
  static constexpr bool is_lock_free() noexcept { return false; }

 private:
  static std::size_t round_up_capacity(std::size_t requested) {
    std::size_t capacity = 2;
    while (capacity < requested) capacity <<= 1;
    return capacity;
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> cells_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
/// Single-threaded Web host without SharedArrayBuffer: no concurrent consumer.
inline constexpr bool kSliderQueueLockFree = false;
#else
inline constexpr bool kSliderQueueLockFree = true;
#endif

template <bool LockFree>
using BasicSliderUpdateQueue = BasicBoundedQueue<SliderUpdate, LockFree>;

using SliderUpdateQueue = BasicSliderUpdateQueue<kSliderQueueLockFree>;

}  // namespace jsfx
