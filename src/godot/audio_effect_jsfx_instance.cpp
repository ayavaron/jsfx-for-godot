#include "audio_effect_jsfx_instance.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace jsfx_godot {
namespace {

static_assert(sizeof(godot::AudioFrame) == sizeof(jsfx::AudioFrame),
              "godot::AudioFrame and jsfx::AudioFrame layouts diverged");
static_assert(alignof(godot::AudioFrame) == alignof(jsfx::AudioFrame),
              "godot::AudioFrame and jsfx::AudioFrame alignment diverged");

}  // namespace

AudioEffectJSFXInstance::AudioEffectJSFXInstance() = default;

AudioEffectJSFXInstance::AudioEffectJSFXInstance(
    Ref<AudioEffectJSFX> effect, std::unique_ptr<jsfx::JsfxProcessor> processor,
    int downsample_factor, bool force_mono)
    : effect_(std::move(effect)),
      processor_(processor.release()),
      downsample_factor_(downsample_factor),
      force_mono_(force_mono) {
  if (effect_.is_valid()) effect_->register_instance(this);
}

AudioEffectJSFXInstance::~AudioEffectJSFXInstance() {
  if (effect_.is_valid()) effect_->unregister_instance(this);

  jsfx::JsfxProcessor* current =
      processor_.exchange(nullptr, std::memory_order_acq_rel);
  delete current;

  for (RetiredProcessor& retired : retired_) delete retired.processor;
  retired_.clear();
}

void AudioEffectJSFXInstance::_bind_methods() {
  // Deferred reclamation target; internal class, not exposed to scripts.
  ClassDB::bind_method(D_METHOD("cleanup_retired_processors"),
                       &AudioEffectJSFXInstance::cleanup_retired_processors);
}

void AudioEffectJSFXInstance::_process(const void* p_src_buffer,
                                       AudioFrame* p_dst_buffer,
                                       int32_t p_frame_count) {
  if (p_src_buffer == nullptr || p_dst_buffer == nullptr ||
      p_frame_count <= 0) {
    return;
  }

  // Sequentially consistent: pairs with the exchange in swap_processor so the
  // main thread can tell whether this block may still see the old pointer.
  process_epoch_.fetch_add(1, std::memory_order_seq_cst);

  const AudioFrame* source = static_cast<const AudioFrame*>(p_src_buffer);
  jsfx::JsfxProcessor* processor = processor_.load(std::memory_order_seq_cst);

  if (processor == nullptr) {
    if (source != p_dst_buffer) {
      std::copy(source, source + p_frame_count, p_dst_buffer);
    }
    // Keep the queue from filling while the effect is static passthrough.
    jsfx::SliderUpdate discarded;
    while (queue_.pop(discarded)) {
    }
    processed_frames_.fetch_add(static_cast<std::uint64_t>(p_frame_count),
                                std::memory_order_relaxed);
    process_epoch_.fetch_add(1, std::memory_order_release);
    return;
  }

  jsfx::SliderUpdate update;
  while (queue_.pop(update)) {
    processor->set_slider(update.index, update.value);
  }

  processor->set_downsample_factor(
      downsample_factor_.load(std::memory_order_relaxed));
  processor->set_force_mono(force_mono_.load(std::memory_order_relaxed));

  processor->process(
      reinterpret_cast<const jsfx::AudioFrame*>(source),
      reinterpret_cast<jsfx::AudioFrame*>(p_dst_buffer), p_frame_count);

  processed_frames_.fetch_add(static_cast<std::uint64_t>(p_frame_count),
                              std::memory_order_relaxed);
  process_epoch_.fetch_add(1, std::memory_order_release);
}

bool AudioEffectJSFXInstance::push_slider(int index, double value) {
  if (processor_.load(std::memory_order_relaxed) == nullptr) return true;
  return queue_.push(jsfx::SliderUpdate{index, value});
}

void AudioEffectJSFXInstance::set_downsample_factor(int factor) {
  factor = std::clamp(factor, 1, jsfx::JsfxProcessor::kMaxDownsampleFactor);
  downsample_factor_.store(factor, std::memory_order_relaxed);
}

void AudioEffectJSFXInstance::set_force_mono(bool enabled) {
  force_mono_.store(enabled, std::memory_order_relaxed);
}

void AudioEffectJSFXInstance::swap_processor(
    std::unique_ptr<jsfx::JsfxProcessor> processor) {
  jsfx::JsfxProcessor* replacement = processor.release();
  if (replacement == nullptr) return;

  jsfx::JsfxProcessor* old =
      processor_.exchange(replacement, std::memory_order_seq_cst);
  if (old == nullptr) return;

  RetiredProcessor retired;
  retired.processor = old;
  retired.epoch_at_swap = process_epoch_.load(std::memory_order_seq_cst);
  if (can_reclaim(retired)) {
    delete old;  // the audio thread was not inside _process
    return;
  }
  retired_.push_back(retired);
  schedule_retired_cleanup();
}

bool AudioEffectJSFXInstance::can_reclaim(
    const RetiredProcessor& retired) const {
  // Even epoch at the swap: the audio thread was outside _process, and its
  // next block loads the new pointer. Otherwise wait for the epoch to move,
  // which means the block that may have loaded the old pointer has finished.
  if ((retired.epoch_at_swap & 1u) == 0) return true;
  return process_epoch_.load(std::memory_order_acquire) != retired.epoch_at_swap;
}

void AudioEffectJSFXInstance::schedule_retired_cleanup() {
  if (cleanup_scheduled_) return;

  // Check again next frame. A deferred call would run again within the same
  // message-queue flush and spin while the audio block is still running.
  Engine* engine = Engine::get_singleton();
  SceneTree* tree =
      engine != nullptr ? Object::cast_to<SceneTree>(engine->get_main_loop())
                        : nullptr;
  if (tree == nullptr) return;  // reclaimed in the destructor instead
  cleanup_scheduled_ = true;
  tree->connect("process_frame",
                Callable(this, "cleanup_retired_processors"),
                Object::CONNECT_ONE_SHOT);
}

void AudioEffectJSFXInstance::cleanup_retired_processors() {
  cleanup_scheduled_ = false;
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (can_reclaim(*it)) {
      delete it->processor;
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
  if (!retired_.empty()) schedule_retired_cleanup();
}

}  // namespace jsfx_godot
