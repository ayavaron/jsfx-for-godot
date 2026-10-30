#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "jsfx/midi.h"
#include "jsfx/processor.h"
#include "jsfx/slider_update_queue.h"

namespace jsfx_godot {

/// The audio-thread side of one running JSFX: its processor, the queues that
/// carry slider values and MIDI to it (and MIDI back), and hot-swap with safe
/// reclamation. Bus effects (AudioEffectJSFXInstance) and instruments
/// (AudioStreamPlaybackJSFX) each own one. A null processor is passthrough
/// (missing or broken script).
class JsfxRuntime {
 public:
  JsfxRuntime(std::unique_ptr<jsfx::JsfxProcessor> processor,
              int downsample_factor, bool force_mono);
  ~JsfxRuntime();

  JsfxRuntime(const JsfxRuntime&) = delete;
  JsfxRuntime& operator=(const JsfxRuntime&) = delete;

  /// Audio thread. No locks or allocation. May run in place.
  void process(const godot::AudioFrame* source, godot::AudioFrame* destination,
               int frame_count);

  /// Any non-audio thread. Return false when the queue is full.
  bool push_slider(int index, double value);
  bool push_midi(const jsfx::MidiMessage& message);
  void set_downsample_factor(int factor);
  void set_force_mono(bool enabled);

  bool has_processor() const;
  std::uint64_t processed_frames() const;

  /// Main thread: publish a compiled replacement. Returns true if the old
  /// processor is still waiting to be reclaimed (call `reclaim` later).
  bool swap_processor(std::unique_ptr<jsfx::JsfxProcessor> processor);

  /// Main thread: free retired processors the audio thread can no longer
  /// reach. Returns true while some are still pending.
  bool reclaim();

  /// Main thread: hands every message the script sent with `midisend` since
  /// the last call to `callback(const jsfx::MidiMessage&)`.
  template <typename Callback>
  void drain_midi_output(Callback&& callback) {
    jsfx::MidiMessage message;
    while (midi_out_.pop(message)) callback(message);
  }

 private:
  struct RetiredProcessor {
    jsfx::JsfxProcessor* processor = nullptr;
    /// `process_epoch_` observed right after the swap.
    std::uint64_t epoch_at_swap = 0;
  };

  using MidiQueue =
      jsfx::BasicBoundedQueue<jsfx::MidiMessage, jsfx::kSliderQueueLockFree>;

  bool can_reclaim(const RetiredProcessor& retired) const;

  std::atomic<jsfx::JsfxProcessor*> processor_{nullptr};
  /// Incremented on entry to and exit from `process`, so it is odd while the
  /// audio thread may be using a processor pointer it loaded.
  std::atomic<std::uint64_t> process_epoch_{0};
  std::atomic<std::uint64_t> processed_frames_{0};
  std::vector<RetiredProcessor> retired_;

  jsfx::SliderUpdateQueue slider_queue_;
  MidiQueue midi_in_{1024};
  MidiQueue midi_out_{1024};
  std::atomic<int> downsample_factor_{1};
  std::atomic<bool> force_mono_{false};
};

/// Main thread: call `target->method()` once on the next frame (SceneTree
/// `process_frame`). Returns false when there is no SceneTree; callers then
/// leave the work to their destructor.
bool connect_next_frame(godot::Object* target, const godot::StringName& method);

/// Implemented by the Godot objects that own a JsfxRuntime, so the
/// AudioEffectJSFX resource can broadcast to and hot-reload all of them.
class JsfxRuntimeOwner {
 public:
  virtual JsfxRuntime& jsfx_runtime() = 0;

  /// Main thread: arrange for `jsfx_runtime().reclaim()` to run on later
  /// frames until it returns false.
  virtual void schedule_reclaim() = 0;

 protected:
  ~JsfxRuntimeOwner() = default;
};

}  // namespace jsfx_godot
