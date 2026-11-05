// Strings, printf, the `<? ?>` preprocessor, mdct, atomics and the
// file_open() family.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jsfx/file_data.h"
#include "jsfx/processor.h"

using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Serves files from memory and counts how often a file had to be opened.
class MemoryProvider final : public jsfx::JsfxFileProvider {
 public:
  std::map<std::string, std::vector<std::uint8_t>, std::less<>> files;
  int opens = 0;

  std::shared_ptr<const jsfx::JsfxFileData> open(std::string_view name) override {
    const auto it = files.find(name);
    if (it == files.end()) return nullptr;
    ++opens;
    return jsfx::decode_file(it->second.data(), it->second.size(), name);
  }

  std::vector<std::string> list(std::string_view folder) override {
    std::vector<std::string> names;
    const std::string prefix = std::string(folder) + "/";
    for (const auto& [name, bytes] : files) {
      if (name.rfind(prefix, 0) == 0) names.push_back(name.substr(prefix.size()));
    }
    return names;  // std::map keeps them sorted
  }
};

std::vector<std::uint8_t> text_bytes(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

void put16(std::vector<std::uint8_t>& out, int value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

// A 16-bit PCM WAV: channel c of frame f holds (f + 1) * 100 * (c ? -1 : 1).
std::vector<std::uint8_t> make_wav(int channels, int rate, int frames) {
  std::vector<std::uint8_t> out;
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(frames * channels * 2);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put32(out, 36 + data_bytes);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(out, 16);
  put16(out, 1);  // PCM
  put16(out, channels);
  put32(out, static_cast<std::uint32_t>(rate));
  put32(out, static_cast<std::uint32_t>(rate * channels * 2));
  put16(out, channels * 2);
  put16(out, 16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put32(out, data_bytes);
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) put16(out, (f + 1) * 100 * (c ? -1 : 1));
  }
  return out;
}

std::vector<std::uint8_t> read_fixture(const char* name) {
  const std::filesystem::path path =
      std::filesystem::path(JSFX_TESTS_DIR) / "fixtures" / name;
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.good());
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
}

std::unique_ptr<jsfx::JsfxProcessor> make(std::string_view source,
                                          jsfx::JsfxEnvironment environment = {}) {
  auto created = jsfx::JsfxProcessor::create(source, environment);
  if (!created.ok()) FAIL(created.error().message << " (line " << created.error().line << ")");
  return std::move(created.value());
}

double var(jsfx::JsfxProcessor& processor, const char* name) {
  double* value = processor.variable(name);
  REQUIRE(value != nullptr);
  return *value;
}

void run_block(jsfx::JsfxProcessor& processor, int frames = 64) {
  std::vector<jsfx::AudioFrame> buffer(static_cast<std::size_t>(frames));
  processor.process(buffer.data(), buffer.data(), frames);
}

}  // namespace

TEST_CASE("strings: literals, user and named strings, sprintf and match",
          "[strings]") {
  auto processor = make(
      "@init\n"
      "strcpy(0, \"hello\");\n"
      "strcat(0, \" world\");\n"
      "len = strlen(0);\n"
      "sprintf(#msg, \"%d-%.1f\", 7, 2.5);\n"
      "same = strcmp(#msg, \"7-2.5\") == 0;\n"
      "found = match(\"*wor*\", 0);\n"
      "ch = str_getchar(0, 1);\n"
      "unnamed = sprintf(#, \"%s!\", \"hi\");\n"
      "ulen = strlen(unnamed);\n");
  CHECK(var(*processor, "len") == 11);
  CHECK(var(*processor, "same") == 1);
  CHECK(var(*processor, "found") == 1);
  CHECK(var(*processor, "ch") == 'e');
  CHECK(var(*processor, "ulen") == 3);
}

TEST_CASE("printf output reaches the host log", "[strings]") {
  auto processor = make(
      "@init\n"
      "printf(\"init %d\\n\", 3);\n"
      "@block\n"
      "printf(\"block\\n\");\n");
  std::string log;
  REQUIRE(processor->drain_log(log));
  CHECK(log == "init 3\n");
  run_block(*processor);  // printf on the audio thread goes through the ring
  log.clear();
  REQUIRE(processor->drain_log(log));
  CHECK(log == "block\n");
  log.clear();
  CHECK_FALSE(processor->drain_log(log));
}

TEST_CASE("the <? ?> preprocessor generates code and keeps line numbers",
          "[preprocessor]") {
  auto processor = make(
      "desc: generated\n"
      "config: count \"Count\" 3 1 2 3 4\n"
      "@init\n"
      "<? loop(x=0;count, printf(\"v%d = %d;\\n\", x, (x + 1) * 10); x += 1) ?>\n"
      "total = v0 + v1 + v2;\n");
  CHECK(var(*processor, "total") == 60);

  // An error after a block that generated three lines still reports the line
  // in the original file.
  auto failed = jsfx::JsfxProcessor::create(
      "@init\n"
      "<? loop(x=0;3, printf(\"a%d = 1;\\n\", x); x += 1) ?>\n"
      "ok = 1;\n"
      "broken = (;\n");
  REQUIRE_FALSE(failed.ok());
  CHECK(failed.error().line == 4);
  CHECK(failed.error().message.find("@init: 4:") != std::string::npos);
}

TEST_CASE("mdct and imdct run without allocating on the audio thread",
          "[mdct]") {
  auto processor = make(
      "@init\n"
      "n = 256;\n"
      "@block\n"
      "i = 0; loop(n * 2, buf[i] = sin(i * 0.1); i += 1);\n"
      "mdct(buf, n * 2);\n"
      "energy = 0; i = 0; loop(n, energy += buf[i] * buf[i]; i += 1);\n");
  run_block(*processor);
  CHECK(var(*processor, "energy") > 0.0);
}

TEST_CASE("atomic functions work on plain variables", "[atomic]") {
  auto processor = make(
      "@init\n"
      "a = 1; atomic_add(a, 2);\n"
      "b = 5; old = atomic_exch(a, b);\n"
      "atomic_set(c, 9); got = atomic_get(c);\n");
  CHECK(var(*processor, "a") == 5);
  CHECK(var(*processor, "b") == 3);
  CHECK(var(*processor, "got") == 9);
}

TEST_CASE("midirecv_buf/_str and midisend_buf/_str carry short messages",
          "[midi]") {
  auto processor = make(
      "@block\n"
      "n = 0;\n"
      "while ((len = midirecv_buf(ofs, 100, 16)) > 0) (\n"
      "  n += 1; last_len = len; b0 = 100[0]; b1 = 100[1]; b2 = 100[2];\n"
      "  midisend_buf(ofs, 100, len);\n"
      ");\n");
  processor->add_midi_input({0, 0x91, 60, 100});
  processor->add_midi_input({5, 0xC2, 7, 0});
  processor->add_midi_input({9, 0x80, 61, 0});
  run_block(*processor);
  CHECK(var(*processor, "n") == 3);
  CHECK(var(*processor, "last_len") == 3);
  CHECK(var(*processor, "b0") == 0x80);
  CHECK(var(*processor, "b1") == 61);
  const auto sent = processor->midi_output();
  REQUIRE(sent.size() == 3);
  CHECK(sent[0].status == 0x91);
  CHECK(sent[1].status == 0xC2);
  CHECK(sent[1].data1 == 7);
  CHECK(sent[1].offset == 5);

  auto strings = make(
      "@block\n"
      "midirecv_str(ofs, #m) ? (\n"
      "  s_len = strlen(#m); s0 = str_getchar(#m, 0, 'cu');\n"
      "  midisend_str(ofs, #m);\n"
      ");\n");
  strings->add_midi_input({3, 0xB0, 7, 99});
  run_block(*strings);
  CHECK(var(*strings, "s_len") == 3);
  CHECK(var(*strings, "s0") == 0xB0);
  REQUIRE(strings->midi_output().size() == 1);
  CHECK(strings->midi_output()[0].data2 == 99);
  CHECK(strings->midi_output()[0].offset == 3);
}

TEST_CASE("file_open reads WAV samples with file_riff and file_mem", "[files]") {
  auto provider = std::make_shared<MemoryProvider>();
  provider->files["samples/two.wav"] = make_wav(2, 22050, 4);
  jsfx::JsfxEnvironment environment;
  environment.file_provider = provider;

  auto processor = make(
      "@init\n"
      "h = file_open(\"samples/two.wav\");\n"
      "file_riff(h, nch, sr);\n"
      "avail = file_avail(h);\n"
      "got = file_mem(h, 100, 8);\n"
      "left_after = file_avail(h);\n"
      "file_rewind(h); file_var(h, first);\n"
      "closed = file_close(h);\n"
      "missing = file_open(\"nope.wav\");\n"
      "right0 = 100[1];\n",
      environment);
  CHECK(var(*processor, "h") >= 0);
  CHECK(var(*processor, "nch") == 2);
  CHECK(var(*processor, "sr") == 22050);
  CHECK(var(*processor, "avail") == 8);
  CHECK(var(*processor, "got") == 8);
  CHECK(var(*processor, "left_after") == 0);
  CHECK(var(*processor, "first") == Approx(100.0 / 32768.0));
  CHECK(var(*processor, "closed") == 0);
  CHECK(var(*processor, "missing") < 0);
  CHECK(var(*processor, "right0") == Approx(-100.0 / 32768.0));  // interleaved
}

TEST_CASE("FLAC, Ogg Vorbis and MP3 decode to the expected signal", "[files]") {
  struct Case {
    const char* name;
    int channels;
  };
  for (const Case& c : {Case{"sine_mono.flac", 1}, Case{"sine_stereo.ogg", 2},
                        Case{"sine_mono.mp3", 1}}) {
    INFO(c.name);
    const std::vector<std::uint8_t> bytes = read_fixture(c.name);
    const auto data = jsfx::decode_file(bytes.data(), bytes.size(), c.name);
    REQUIRE(data->kind == jsfx::JsfxFileData::Kind::Audio);
    CHECK(data->channels == c.channels);
    CHECK(data->sample_rate == 44100);
    const std::size_t frames = data->values.size() / static_cast<std::size_t>(c.channels);
    // 0.1 s; MP3 adds encoder padding.
    CHECK(frames >= 4300);
    CHECK(frames <= 6000);
    // A 0.5-amplitude sine has RMS 0.354 (lossy codecs land close to it).
    double sum = 0.0;
    for (double v : data->values) sum += v * v;
    CHECK(std::sqrt(sum / data->values.size()) == Approx(0.354).margin(0.04));
  }
}

TEST_CASE("text files read as numbers, and lines through file_string", "[files]") {
  auto provider = std::make_shared<MemoryProvider>();
  provider->files["map.txt"] = text_bytes("# map\n60 62, 64\r\nlast -1.5\n");
  jsfx::JsfxEnvironment environment;
  environment.file_provider = provider;
  auto processor = make(
      "@init\n"
      "h = file_open(\"map.txt\");\n"
      "is_text = file_text(h);\n"
      "count = file_avail(h);\n"
      "file_var(h, a); file_var(h, b);\n"
      "n = file_mem(h, 10, 100);\n"
      "c = 10[0]; d = 10[1];\n"
      "file_rewind(h);\n"
      "file_string(h, #line1); file_string(h, #line2);\n"
      "len2 = strlen(#line2);\n",
      environment);
  CHECK(var(*processor, "is_text") == 1);
  CHECK(var(*processor, "count") == 4);
  CHECK(var(*processor, "a") == 60);
  CHECK(var(*processor, "b") == 62);
  CHECK(var(*processor, "n") == 2);
  CHECK(var(*processor, "c") == 64);
  CHECK(var(*processor, "d") == -1.5);
  CHECK(var(*processor, "len2") == std::strlen("60 62, 64"));
}

TEST_CASE("file sliders pick from the folder listing", "[files]") {
  auto provider = std::make_shared<MemoryProvider>();
  provider->files["models/a.txt"] = text_bytes("1");
  provider->files["models/b.txt"] = text_bytes("2");
  jsfx::JsfxEnvironment environment;
  environment.file_provider = provider;
  environment.file_cache = std::make_shared<jsfx::JsfxFileCache>();
  auto processor = make(
      "slider1:/models:b.txt:Model\n"
      "@slider\n"
      "h = file_open(slider1);\n"
      "h >= 0 ? (file_var(h, value); file_close(h)) : value = -1;\n",
      environment);
  CHECK(var(*processor, "slider1") == 1);  // "b.txt" is the second file
  CHECK(var(*processor, "value") == 2);

  // Picking another file from the host: preload it (main thread), then the
  // audio thread's @slider finds it in the shared cache.
  environment.file_cache->find_or_open("models/a.txt", provider.get());
  processor->set_slider(1, 0);
  run_block(*processor);
  CHECK(var(*processor, "value") == 1);
}

TEST_CASE("file_open on the audio thread only uses preloaded files", "[files]") {
  auto provider = std::make_shared<MemoryProvider>();
  provider->files["late.txt"] = text_bytes("5");
  jsfx::JsfxEnvironment environment;
  environment.file_provider = provider;
  environment.file_cache = std::make_shared<jsfx::JsfxFileCache>();
  auto processor = make(
      "@block\n"
      "h = file_open(\"late.txt\");\n"
      "h >= 0 ? (file_var(h, value); file_close(h));\n",
      environment);
  run_block(*processor);
  CHECK(var(*processor, "h") < 0);
  CHECK(provider->opens == 0);  // no I/O from the audio thread
  std::string missed;
  REQUIRE(processor->take_missed_file(missed));
  CHECK(missed == "late.txt");

  environment.file_cache->find_or_open(missed, provider.get());
  run_block(*processor);
  CHECK(var(*processor, "value") == 5);
}

TEST_CASE("contexts sharing a file cache decode each file once", "[files]") {
  auto provider = std::make_shared<MemoryProvider>();
  provider->files["one.txt"] = text_bytes("1");
  jsfx::JsfxEnvironment environment;
  environment.file_provider = provider;
  environment.file_cache = std::make_shared<jsfx::JsfxFileCache>();
  const char* script = "@init\nh = file_open(\"one.txt\"); file_var(h, v);\n";
  auto first = make(script, environment);
  auto second = make(script, environment);
  CHECK(var(*first, "v") == 1);
  CHECK(var(*second, "v") == 1);
  CHECK(provider->opens == 1);
}

TEST_CASE("the directory provider lists and opens files", "[files]") {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "jsfx_files_test";
  fs::remove_all(root);
  fs::create_directories(root / "kit");
  for (const char* name : {"Snare.txt", "kick.txt", ".hidden"}) {
    std::ofstream(root / "kit" / name) << "3";
  }
  auto provider = jsfx::make_directory_file_provider({(root / "missing").string(),
                                                      root.string()});
  const std::vector<std::string> names = provider->list("kit");
  REQUIRE(names.size() == 2);
  CHECK(names[0] == "kick.txt");  // case-insensitive order, dotfiles hidden
  CHECK(names[1] == "Snare.txt");
  auto data = provider->open("kit/kick.txt");
  REQUIRE(data != nullptr);
  CHECK(data->values == std::vector<double>{3.0});
  CHECK(provider->open("kit/none.txt") == nullptr);
  fs::remove_all(root);
}
