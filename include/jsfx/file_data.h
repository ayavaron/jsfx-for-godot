#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jsfx {

/// One file a script can read with `file_open`, decoded once on the main
/// thread and then shared, read-only, by every context that opens it.
struct JsfxFileData {
  enum class Kind { Audio, Text };

  Kind kind = Kind::Text;

  /// Audio (WAV, FLAC, Ogg Vorbis, MP3): interleaved samples in -1..1.
  int channels = 0;
  double sample_rate = 0.0;

  /// Audio samples, or the numbers of a text file, in read order.
  std::vector<double> values;

  /// Text files: the raw bytes, NUL-terminated, for `file_string`.
  std::string text;
};

/// Decodes file bytes. Audio is recognised by content (RIFF/WAVE, fLaC, OggS,
/// MP3 frames or ID3) or by a `.mp3` name; anything else is read as text,
/// REAPER-style: numbers separated by whitespace, commas or other non-numeric
/// characters. Main thread only (allocates). Never returns null.
std::shared_ptr<const JsfxFileData> decode_file(const std::uint8_t* bytes,
                                                std::size_t size,
                                                std::string_view name);

/// Where a script's files come from. Implementations run on the main thread
/// only: contexts call them during `@init`/the first `@slider`, and hosts call
/// them to preload a file before a file slider changes on the audio thread.
class JsfxFileProvider {
 public:
  virtual ~JsfxFileProvider() = default;

  /// The decoded file for a script-relative name such as "DrNine/Kick.flac"
  /// or "amp_models/Fender Bassman.wav", or null if it cannot be found.
  virtual std::shared_ptr<const JsfxFileData> open(std::string_view name) = 0;

  /// The sorted file names in a file slider's folder (e.g. "amp_models"),
  /// without the folder. Used for `sliderN:/folder:default:desc`.
  virtual std::vector<std::string> list(std::string_view folder) = 0;
};

/// A provider over plain directories, searched in order. Used by the
/// benchmark and tests; Godot uses its own provider over `res://`.
std::shared_ptr<JsfxFileProvider> make_directory_file_provider(
    std::vector<std::string> directories);

/// Decoded files shared by every context of one effect. Files are only ever
/// added, by the main thread; the audio thread looks them up without locks or
/// allocation. A full cache refuses new files.
class JsfxFileCache {
 public:
  static constexpr int kCapacity = 128;

  /// Adds (or finds) `name`. Main thread only.
  const JsfxFileData* add(std::string_view name,
                          std::shared_ptr<const JsfxFileData> data);

  /// Real-time safe lookup; null if the file has not been loaded.
  const JsfxFileData* find(std::string_view name) const;

  /// Number of files loaded so far.
  int size() const { return count_.load(std::memory_order_acquire); }

  /// Main-thread helper: the cached file, or opens it through `provider`.
  const JsfxFileData* find_or_open(std::string_view name,
                                   JsfxFileProvider* provider);

 private:
  struct Entry {
    std::string name;
    std::shared_ptr<const JsfxFileData> data;
  };
  Entry entries_[kCapacity];
  std::atomic<int> count_{0};
};

/// Normalises a script file name for lookups: backslashes become slashes,
/// leading "./" and "/" are dropped. Allocation-free when `out` is large
/// enough; returns false if the name does not fit.
bool normalize_file_name(std::string_view name, char* out, std::size_t out_size);

}  // namespace jsfx
