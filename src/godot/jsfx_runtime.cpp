#include "jsfx_runtime.h"

#include <algorithm>
#include <utility>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/callable.hpp>

namespace jsfx_godot {
namespace {

static_assert(sizeof(godot::AudioFrame) == sizeof(jsfx::AudioFrame),
              "godot::AudioFrame and jsfx::AudioFrame layouts diverged");
static_assert(alignof(godot::AudioFrame) == alignof(jsfx::AudioFrame),
              "godot::AudioFrame and jsfx::AudioFrame alignment diverged");

}  // namespace

bool connect_next_frame(godot::Object* target, const godot::StringName& method) {
  godot::Engine* engine = godot::Engine::get_singleton();
  godot::SceneTree* tree =
      engine != nullptr
          ? godot::Object::cast_to<godot::SceneTree>(engine->get_main_loop())
          : nullptr;
  if (tree == nullptr) return false;
  // A deferred call would run again within the same message-queue flush and
  // spin; one-shot process_frame waits for the next frame.
  return tree->connect("process_frame", godot::Callable(target, method),
                       godot::Object::CONNECT_ONE_SHOT) == godot::OK;
}

JsfxRuntime::JsfxRuntime(std::unique_ptr<jsfx::JsfxProcessor> processor,
                         int downsample_factor, bool force_mono)
    : processor_(processor.release()),
      downsample_factor_(downsample_factor),
      force_mono_(force_mono) {}

JsfxRuntime::~JsfxRuntime() {
  delete processor_.exchange(nullptr, std::memory_order_acq_rel);
  for (RetiredProcessor& retired : retired_) delete retired.processor;
}

void JsfxRuntime::process(const godot::AudioFrame* source,
                          godot::AudioFrame* destination, int frame_count) {
  if (source == nullptr || destination == nullptr || frame_count <= 0) return;

  // Sequentially consistent: pairs with the exchange in swap_processor so the
  // main thread can tell whether this block may still see the old pointer.
  process_epoch_.fetch_add(1, std::memory_order_seq_cst);
  jsfx::JsfxProcessor* processor = processor_.load(std::memory_order_seq_cst);

  if (processor == nullptr) {
    if (source != destination) std::copy(source, source + frame_count, destination);
    // Keep the queues from filling while the effect is static passthrough.
    jsfx::SliderUpdate slider;
    while (slider_queue_.pop(slider)) {
    }
    jsfx::MidiMessage message;
    while (midi_in_.pop(message)) {
    }
  } else {
    jsfx::SliderUpdate slider;
    while (slider_queue_.pop(slider)) {
      processor->set_slider(slider.index, slider.value);
    }
    // At most one block's worth of MIDI; the rest waits for the next block.
    jsfx::MidiMessage message;
    for (int i = 0; i < jsfx::kMaxMidiEventsPerBlock && midi_in_.pop(message); ++i) {
      processor->add_midi_input(message);
    }

    processor->set_downsample_factor(
        downsample_factor_.load(std::memory_order_relaxed));
    processor->set_force_mono(force_mono_.load(std::memory_order_relaxed));
    processor->process(reinterpret_cast<const jsfx::AudioFrame*>(source),
                       reinterpret_cast<jsfx::AudioFrame*>(destination),
                       frame_count);

    for (const jsfx::MidiMessage& sent : processor->midi_output()) {
      midi_out_.push(sent);  // dropped if the main thread falls far behind
    }
  }

  processed_frames_.fetch_add(static_cast<std::uint64_t>(frame_count),
                              std::memory_order_relaxed);
  process_epoch_.fetch_add(1, std::memory_order_release);
}

bool JsfxRuntime::push_slider(int index, double value) {
  if (!has_processor()) return true;
  return slider_queue_.push(jsfx::SliderUpdate{index, value});
}

bool JsfxRuntime::push_midi(const jsfx::MidiMessage& message) {
  if (!has_processor()) return false;
  return midi_in_.push(message);
}

void JsfxRuntime::set_downsample_factor(int factor) {
  factor = std::clamp(factor, 1, jsfx::JsfxProcessor::kMaxDownsampleFactor);
  downsample_factor_.store(factor, std::memory_order_relaxed);
}

void JsfxRuntime::set_force_mono(bool enabled) {
  force_mono_.store(enabled, std::memory_order_relaxed);
}

bool JsfxRuntime::has_processor() const {
  return processor_.load(std::memory_order_relaxed) != nullptr;
}

std::uint64_t JsfxRuntime::processed_frames() const {
  return processed_frames_.load(std::memory_order_relaxed);
}

bool JsfxRuntime::swap_processor(std::unique_ptr<jsfx::JsfxProcessor> processor) {
  jsfx::JsfxProcessor* replacement = processor.release();
  if (replacement == nullptr) return !retired_.empty();

  jsfx::JsfxProcessor* old =
      processor_.exchange(replacement, std::memory_order_seq_cst);
  if (old == nullptr) return !retired_.empty();

  RetiredProcessor retired;
  retired.processor = old;
  retired.epoch_at_swap = process_epoch_.load(std::memory_order_seq_cst);
  if (can_reclaim(retired)) {
    delete old;  // the audio thread was not inside process()
  } else {
    retired_.push_back(retired);
  }
  return !retired_.empty();
}

bool JsfxRuntime::can_reclaim(const RetiredProcessor& retired) const {
  // Even epoch at the swap: the audio thread was outside process(), and its
  // next block loads the new pointer. Otherwise wait for the epoch to move,
  // which means the block that may have loaded the old pointer has finished.
  if ((retired.epoch_at_swap & 1u) == 0) return true;
  return process_epoch_.load(std::memory_order_acquire) != retired.epoch_at_swap;
}

bool JsfxRuntime::reclaim() {
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (can_reclaim(*it)) {
      delete it->processor;
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
  return !retired_.empty();
}

}  // namespace jsfx_godot
