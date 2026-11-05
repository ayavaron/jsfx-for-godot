// Decoding and caching for JSFX's file_open() family. Compiled with the
// vendored libraries (warnings off): dr_wav, dr_flac and dr_mp3 (public domain
// or MIT-0) and stb_vorbis (public domain).
#include "jsfx/file_data.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
// stb_vorbis leaks short macro names into the including file.
#undef L
#undef C
#undef R

namespace jsfx {
namespace {

bool starts_with(const std::uint8_t* bytes, std::size_t size, const char* magic,
                 std::size_t offset = 0) {
  const std::size_t length = std::strlen(magic);
  return size >= offset + length && std::memcmp(bytes + offset, magic, length) == 0;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    const char a = static_cast<char>(
        std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i])));
    if (a != suffix[i]) return false;
  }
  return true;
}

std::shared_ptr<JsfxFileData> audio(int channels, double rate, const float* samples,
                                    std::uint64_t frames) {
  auto data = std::make_shared<JsfxFileData>();
  data->kind = JsfxFileData::Kind::Audio;
  data->channels = channels;
  data->sample_rate = rate;
  data->values.assign(samples, samples + frames * static_cast<std::uint64_t>(channels));
  return data;
}

std::shared_ptr<JsfxFileData> decode_audio(const std::uint8_t* bytes, std::size_t size,
                                           std::string_view name) {
  if (starts_with(bytes, size, "RIFF") && starts_with(bytes, size, "WAVE", 8)) {
    unsigned int channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float* samples = drwav_open_memory_and_read_pcm_frames_f32(
        bytes, size, &channels, &rate, &frames, nullptr);
    if (samples == nullptr) return nullptr;
    auto data = audio(static_cast<int>(channels), rate, samples, frames);
    drwav_free(samples, nullptr);
    return data;
  }
  if (starts_with(bytes, size, "fLaC")) {
    unsigned int channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float* samples = drflac_open_memory_and_read_pcm_frames_f32(
        bytes, size, &channels, &rate, &frames, nullptr);
    if (samples == nullptr) return nullptr;
    auto data = audio(static_cast<int>(channels), rate, samples, frames);
    drflac_free(samples, nullptr);
    return data;
  }
  if (starts_with(bytes, size, "OggS")) {
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(
        bytes, static_cast<int>(size), &error, nullptr);
    if (vorbis == nullptr) return nullptr;
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    const unsigned int frames = stb_vorbis_stream_length_in_samples(vorbis);
    std::vector<float> samples(static_cast<std::size_t>(frames) * info.channels);
    const int read = stb_vorbis_get_samples_float_interleaved(
        vorbis, info.channels, samples.data(), static_cast<int>(samples.size()));
    stb_vorbis_close(vorbis);
    return audio(info.channels, info.sample_rate, samples.data(),
                 static_cast<std::uint64_t>(read));
  }
  const bool mp3_frame = size >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0;
  if (starts_with(bytes, size, "ID3") || mp3_frame || ends_with_ci(name, ".mp3")) {
    drmp3_config config{};
    drmp3_uint64 frames = 0;
    float* samples = drmp3_open_memory_and_read_pcm_frames_f32(
        bytes, size, &config, &frames, nullptr);
    if (samples == nullptr) return nullptr;
    auto data = audio(static_cast<int>(config.channels), config.sampleRate, samples, frames);
    drmp3_free(samples, nullptr);
    return data;
  }
  return nullptr;
}

std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

class DirectoryFileProvider final : public JsfxFileProvider {
 public:
  explicit DirectoryFileProvider(std::vector<std::string> directories)
      : directories_(std::move(directories)) {}

  std::shared_ptr<const JsfxFileData> open(std::string_view name) override {
    namespace fs = std::filesystem;
    for (const std::string& directory : directories_) {
      const fs::path path = fs::u8path(directory) / fs::u8path(std::string(name));
      std::error_code error;
      if (!fs::is_regular_file(path, error)) continue;
      std::ifstream file(path, std::ios::binary);
      if (!file) continue;
      const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
      return decode_file(bytes.data(), bytes.size(), name);
    }
    return nullptr;
  }

  std::vector<std::string> list(std::string_view folder) override {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    for (const std::string& directory : directories_) {
      const fs::path path = fs::u8path(directory) / fs::u8path(std::string(folder));
      std::error_code error;
      if (!fs::is_directory(path, error)) continue;
      for (fs::directory_iterator it(path, error), end; !error && it != end;
           it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const std::u8string utf8 = it->path().filename().u8string();
        std::string file(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        if (!file.empty() && file[0] != '.') names.push_back(std::move(file));
      }
      if (!names.empty()) break;
    }
    sort_file_names(names);
    return names;
  }

  static void sort_file_names(std::vector<std::string>& names) {
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      return to_lower(a) < to_lower(b);
    });
  }

 private:
  std::vector<std::string> directories_;
};

}  // namespace

std::shared_ptr<const JsfxFileData> decode_file(const std::uint8_t* bytes,
                                                std::size_t size,
                                                std::string_view name) {
  if (bytes != nullptr && size > 0) {
    if (auto decoded = decode_audio(bytes, size, name)) return decoded;
  }

  // Text: every number in the file, in order. Anything that is not part of a
  // number separates numbers, as in REAPER.
  auto data = std::make_shared<JsfxFileData>();
  data->kind = JsfxFileData::Kind::Text;
  data->text.assign(reinterpret_cast<const char*>(bytes), bytes != nullptr ? size : 0);
  const char* p = data->text.c_str();
  while (*p != '\0') {
    const char c = *p;
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
      char* end = nullptr;
      const double value = std::strtod(p, &end);
      if (end != p) {
        data->values.push_back(value);
        p = end;
        continue;
      }
    }
    ++p;
  }
  return data;
}

std::shared_ptr<JsfxFileProvider> make_directory_file_provider(
    std::vector<std::string> directories) {
  return std::make_shared<DirectoryFileProvider>(std::move(directories));
}

bool normalize_file_name(std::string_view name, char* out, std::size_t out_size) {
  std::size_t start = 0;
  while (start < name.size()) {
    if (name[start] == '/' || name[start] == '\\') {
      ++start;
    } else if (name[start] == '.' && start + 1 < name.size() &&
               (name[start + 1] == '/' || name[start + 1] == '\\')) {
      start += 2;
    } else {
      break;
    }
  }
  const std::size_t length = name.size() - start;
  if (length + 1 > out_size) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = name[start + i];
    out[i] = c == '\\' ? '/' : c;
  }
  out[length] = '\0';
  return true;
}

const JsfxFileData* JsfxFileCache::add(std::string_view name,
                                       std::shared_ptr<const JsfxFileData> data) {
  if (const JsfxFileData* existing = find(name)) return existing;
  if (data == nullptr) return nullptr;
  const int count = count_.load(std::memory_order_relaxed);
  if (count >= kCapacity) return nullptr;
  entries_[count].name.assign(name.data(), name.size());
  entries_[count].data = std::move(data);
  // Publish only after the entry is complete; readers stop at count_.
  count_.store(count + 1, std::memory_order_release);
  return entries_[count].data.get();
}

const JsfxFileData* JsfxFileCache::find(std::string_view name) const {
  const int count = count_.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    if (entries_[i].name == name) return entries_[i].data.get();
  }
  return nullptr;
}

const JsfxFileData* JsfxFileCache::find_or_open(std::string_view name,
                                                JsfxFileProvider* provider) {
  if (const JsfxFileData* cached = find(name)) return cached;
  if (provider == nullptr) return nullptr;
  return add(name, provider->open(name));
}

}  // namespace jsfx
