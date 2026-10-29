#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include "jsfx/slider_update_queue.h"

namespace {

template <typename Queue>
void check_fifo(Queue& queue) {
  jsfx::SliderUpdate update;
  CHECK_FALSE(queue.pop(update));

  for (int i = 0; i < 3; ++i) {
    CHECK(queue.push(jsfx::SliderUpdate{i + 1, i * 0.5}));
  }
  for (int i = 0; i < 3; ++i) {
    REQUIRE(queue.pop(update));
    CHECK(update.index == i + 1);
    CHECK(update.value == i * 0.5);
  }
  CHECK_FALSE(queue.pop(update));
}

}  // namespace

TEST_CASE("slider queue is FIFO and reports full/empty", "[queue]") {
  jsfx::BasicSliderUpdateQueue<true> lock_free(4);
  CHECK(lock_free.capacity() == 4);
  CHECK(jsfx::BasicSliderUpdateQueue<true>::is_lock_free());
  check_fifo(lock_free);

  jsfx::BasicSliderUpdateQueue<false> single_threaded(3);
  CHECK(single_threaded.capacity() == 4);  // rounded up to a power of two
  CHECK_FALSE(jsfx::BasicSliderUpdateQueue<false>::is_lock_free());
  check_fifo(single_threaded);

  jsfx::BasicSliderUpdateQueue<true> tiny(0);
  CHECK(tiny.capacity() == 2);
  CHECK(tiny.push(jsfx::SliderUpdate{1, 1.0}));
  CHECK(tiny.push(jsfx::SliderUpdate{2, 2.0}));
  CHECK_FALSE(tiny.push(jsfx::SliderUpdate{3, 3.0}));
}

TEST_CASE("single-threaded fallback queue behaves like a bounded ring",
          "[queue]") {
  jsfx::BasicSliderUpdateQueue<false> queue(2);
  CHECK(queue.push(jsfx::SliderUpdate{1, 1.0}));
  CHECK(queue.push(jsfx::SliderUpdate{2, 2.0}));
  CHECK_FALSE(queue.push(jsfx::SliderUpdate{3, 3.0}));

  jsfx::SliderUpdate update;
  REQUIRE(queue.pop(update));
  CHECK(update.index == 1);
  CHECK(queue.push(jsfx::SliderUpdate{3, 3.0}));

  REQUIRE(queue.pop(update));
  CHECK(update.index == 2);
  REQUIRE(queue.pop(update));
  CHECK(update.index == 3);
  CHECK_FALSE(queue.pop(update));
}

TEST_CASE("lock-free slider queue handles concurrent producers", "[queue][stress]") {
  constexpr int kProducers = 4;
  constexpr int kUpdatesPerProducer = 5000;
  constexpr int kTotalUpdates = kProducers * kUpdatesPerProducer;

  jsfx::BasicSliderUpdateQueue<true> queue(256);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::atomic<int> consumed{0};
  std::array<double, kProducers> last_sequence;
  last_sequence.fill(-1.0);

  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }

    jsfx::SliderUpdate update;
    while (consumed.load(std::memory_order_relaxed) < kTotalUpdates &&
           std::chrono::steady_clock::now() < deadline) {
      if (!queue.pop(update)) {
        std::this_thread::yield();
        continue;
      }
      if (update.index < 0 || update.index >= kProducers) {
        failed.store(true);
        break;
      }
      if (update.value != last_sequence[update.index] + 1.0) {
        failed.store(true);
        break;
      }
      last_sequence[update.index] = update.value;
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
    if (consumed.load(std::memory_order_relaxed) != kTotalUpdates) {
      failed.store(true);
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (int sequence = 0; sequence < kUpdatesPerProducer; ++sequence) {
        const jsfx::SliderUpdate update{producer,
                                        static_cast<double>(sequence)};
        while (!queue.push(update)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            failed.store(true);
            return;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread& producer : producers) producer.join();
  consumer.join();

  CHECK_FALSE(failed.load());
  CHECK(consumed.load() == kTotalUpdates);
}
