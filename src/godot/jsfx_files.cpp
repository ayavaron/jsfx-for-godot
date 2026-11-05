#include "jsfx_files.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

namespace jsfx_godot {
namespace {

String to_godot(std::string_view text) {
  return String::utf8(text.data(), static_cast<int64_t>(text.size()));
}

std::string to_std(const String& text) {
  const CharString utf8 = text.utf8();
  return std::string(utf8.get_data(), static_cast<std::size_t>(utf8.length()));
}

std::shared_ptr<const jsfx::JsfxFileData> decode_bytes(const PackedByteArray& bytes,
                                                       std::string_view name) {
  return jsfx::decode_file(bytes.ptr(), static_cast<std::size_t>(bytes.size()), name);
}

/// Audio Godot imported (so the original may be missing from an export).
std::shared_ptr<const jsfx::JsfxFileData> from_imported(const String& path,
                                                        std::string_view name) {
  ResourceLoader* loader = ResourceLoader::get_singleton();
  if (loader == nullptr || !loader->exists(path)) return nullptr;
  const Ref<Resource> resource = loader->load(path);

  if (const Ref<AudioStreamMP3> mp3 = resource; mp3.is_valid()) {
    return decode_bytes(mp3->get_data(), ".mp3");
  }

  const Ref<AudioStreamWAV> wav = resource;
  if (wav.is_null()) return nullptr;
  const AudioStreamWAV::Format format = wav->get_format();
  if (format != AudioStreamWAV::FORMAT_8_BITS && format != AudioStreamWAV::FORMAT_16_BITS) {
    ERR_PRINT(String("JSFX: '") + path +
              "' was imported compressed; set its import to Keep File or "
              "Compress Mode Disabled so file_open can read it");
    return nullptr;
  }
  const PackedByteArray data = wav->get_data();
  auto decoded = std::make_shared<jsfx::JsfxFileData>();
  decoded->kind = jsfx::JsfxFileData::Kind::Audio;
  decoded->channels = wav->is_stereo() ? 2 : 1;
  decoded->sample_rate = wav->get_mix_rate();
  if (format == AudioStreamWAV::FORMAT_8_BITS) {
    decoded->values.reserve(static_cast<std::size_t>(data.size()));
    for (int64_t i = 0; i < data.size(); ++i) {
      decoded->values.push_back(static_cast<int8_t>(data[i]) / 128.0);
    }
  } else {
    decoded->values.reserve(static_cast<std::size_t>(data.size() / 2));
    for (int64_t i = 0; i + 1 < data.size(); i += 2) {
      const auto sample = static_cast<int16_t>(data[i] | (data[i + 1] << 8));
      decoded->values.push_back(sample / 32768.0);
    }
  }
  (void)name;
  return decoded;
}

class GodotFileProvider final : public jsfx::JsfxFileProvider {
 public:
  explicit GodotFileProvider(const String& script_path)
      : script_dir_(script_path.get_base_dir()) {}

  std::shared_ptr<const jsfx::JsfxFileData> open(std::string_view name) override {
    const String relative = to_godot(name);
    for (const String& base : bases()) {
      const String path = base.path_join(relative);
      if (FileAccess::file_exists(path)) {
        return decode_bytes(FileAccess::get_file_as_bytes(path), name);
      }
    }
    for (const String& base : bases()) {
      if (auto imported = from_imported(base.path_join(relative), name)) return imported;
    }
    return nullptr;
  }

  std::vector<std::string> list(std::string_view folder) override {
    std::vector<std::string> names;
    for (const String& base : bases()) {
      const PackedStringArray files = DirAccess::get_files_at(base.path_join(to_godot(folder)));
      for (int64_t i = 0; i < files.size(); ++i) {
        String file = files[i];
        // Exports keep imported files as "name.wav.import" / ".remap".
        if (file.ends_with(".import")) file = file.trim_suffix(".import");
        if (file.ends_with(".remap")) file = file.trim_suffix(".remap");
        if (file.ends_with(".uid") || file.begins_with(".")) continue;
        const std::string entry = to_std(file);
        if (std::find(names.begin(), names.end(), entry) == names.end()) {
          names.push_back(entry);
        }
      }
      if (!names.empty()) break;
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      return to_godot(a).nocasecmp_to(to_godot(b)) < 0;
    });
    return names;
  }

 private:
  std::vector<String> bases() const {
    String data_path = "res://";
    if (ProjectSettings* settings = ProjectSettings::get_singleton()) {
      data_path = settings->get_setting(kDataPathSetting, "res://");
    }
    std::vector<String> result{script_dir_};
    for (const String& extra : {data_path, String("res://")}) {
      if (std::find(result.begin(), result.end(), extra) == result.end()) {
        result.push_back(extra);
      }
    }
    return result;
  }

  String script_dir_;
};

}  // namespace

void register_data_path_setting() {
  ProjectSettings* settings = ProjectSettings::get_singleton();
  if (settings == nullptr) return;
  if (!settings->has_setting(kDataPathSetting)) {
    settings->set_setting(kDataPathSetting, "res://");
  }
  settings->set_initial_value(kDataPathSetting, "res://");
  Dictionary info;
  info["name"] = kDataPathSetting;
  info["type"] = Variant::STRING;
  info["hint"] = PROPERTY_HINT_DIR;
  settings->add_property_info(info);
}

std::shared_ptr<jsfx::JsfxFileProvider> make_godot_file_provider(
    const String& script_path) {
  return std::make_shared<GodotFileProvider>(script_path);
}

}  // namespace jsfx_godot
