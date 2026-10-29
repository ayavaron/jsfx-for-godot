// jsfx_bench: compatibility and performance probe for the headless core.
//
// For each .jsfx file: parse, compile, run @init, then process white noise in
// fixed blocks and report the cost per block. Godot is not involved, so the
// numbers isolate the JSFX engine from the rest of the mixer.
//
//   jsfx_bench [--seconds S] [--block N] [--rate HZ] [--downsample F]
//              [--mono] [--import-dir DIR]... [--memory N] file.jsfx...
//
// --memory N creates N live instances of the first file and reports the
// committed and resident memory each one adds.
//
// `import` lines resolve against the script's folder, then each --import-dir.
//
// Output is one tab-separated line per file:
//   OK      <file>  <create_ms>  <us_per_block>  <realtime_pct>  <peak>
//   FAIL    <file>  <line>  <message>
//   NONFINITE <file> ...   (output contained NaN/Inf)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "jsfx/jsfx.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {

struct MemoryUsage {
  double committed_mb = 0.0;  // private bytes the OS has promised the process
  double resident_mb = 0.0;   // pages actually in RAM
};

MemoryUsage memory_usage() {
  MemoryUsage usage;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    usage.committed_mb = counters.PrivateUsage / 1048576.0;
    usage.resident_mb = counters.WorkingSetSize / 1048576.0;
  }
#elif defined(__linux__)
  long pages = 0, resident = 0;
  if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(statm, "%ld %ld", &pages, &resident) != 2) resident = 0;
    std::fclose(statm);
  }
  const double page_mb = sysconf(_SC_PAGESIZE) / 1048576.0;
  usage.committed_mb = pages * page_mb;
  usage.resident_mb = resident * page_mb;
#endif
  return usage;
}

bool read_file(const char* path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 2.0;
  int block = 512;
  double rate = 48000.0;
  int downsample = 1;
  bool mono = false;
  std::vector<const char*> files;
  std::vector<std::filesystem::path> import_dirs;
  int memory_instances = 0;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) {
      block = std::max(1, std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) {
      rate = std::atof(argv[++i]);
    } else if (!std::strcmp(argv[i], "--downsample") && i + 1 < argc) {
      downsample = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--import-dir") && i + 1 < argc) {
      import_dirs.emplace_back(argv[++i]);
    } else if (!std::strcmp(argv[i], "--memory") && i + 1 < argc) {
      memory_instances = std::max(1, std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--mono")) {
      mono = true;
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: jsfx_bench [options] file.jsfx...\n");
    return 2;
  }

  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
  std::vector<jsfx::AudioFrame> input(static_cast<std::size_t>(block));
  std::vector<jsfx::AudioFrame> output(static_cast<std::size_t>(block));
  for (jsfx::AudioFrame& frame : input) {
    frame.left = noise(rng);
    frame.right = noise(rng);
  }

  if (memory_instances > 0) {
    std::string source;
    if (!read_file(files[0], source)) return 2;
    jsfx::JsfxEnvironment environment;
    environment.sample_rate = rate;
    std::vector<std::unique_ptr<jsfx::JsfxProcessor>> live;
    const MemoryUsage before = memory_usage();
    for (int i = 0; i < memory_instances; ++i) {
      auto created = jsfx::JsfxProcessor::create(source, environment, files[0]);
      if (!created.ok()) return 1;
      created.value()->process(input.data(), output.data(), block);
      live.push_back(std::move(created.value()));
    }
    const MemoryUsage after = memory_usage();
    std::printf("MEMORY\t%s\tinstances=%d\tcommitted_mb_each=%.2f\t"
                "resident_mb_each=%.2f\n",
                files[0], memory_instances,
                (after.committed_mb - before.committed_mb) / memory_instances,
                (after.resident_mb - before.resident_mb) / memory_instances);
    return 0;
  }

  const int blocks = std::max(1, static_cast<int>(seconds * rate / block));
  int failures = 0;

  for (const char* path : files) {
    std::string source;
    if (!read_file(path, source)) {
      std::printf("FAIL\t%s\t0\tcannot read file\n", path);
      ++failures;
      continue;
    }

    jsfx::JsfxEnvironment environment;
    // Downsampled scripts see the reduced rate, as in the Godot bridge.
    environment.sample_rate = rate / std::clamp(downsample, 1, 8);

    std::vector<std::filesystem::path> search{
        std::filesystem::path(path).parent_path()};
    search.insert(search.end(), import_dirs.begin(), import_dirs.end());
    const jsfx::JsfxImportResolver resolver =
        [&search](std::string_view name) -> std::optional<std::string> {
      for (const std::filesystem::path& dir : search) {
        std::string text;
        if (read_file((dir / std::string(name)).string().c_str(), text)) {
          return text;
        }
      }
      return std::nullopt;
    };

    const auto create_start = std::chrono::steady_clock::now();
    auto parsed = jsfx::JsfxDocument::parse(source, path, resolver);
    if (!parsed.ok()) {
      std::printf("FAIL\t%s\t%d\t%s\n", path, parsed.error().line,
                  parsed.error().message.c_str());
      ++failures;
      continue;
    }
    auto created =
        jsfx::JsfxProcessor::create(std::move(parsed.value()), environment);
    const auto create_end = std::chrono::steady_clock::now();
    if (!created.ok()) {
      std::printf("FAIL\t%s\t%d\t%s\n", path, created.error().line,
                  created.error().message.c_str());
      ++failures;
      continue;
    }
    jsfx::JsfxProcessor& processor = *created.value();
    processor.set_downsample_factor(downsample);
    processor.set_force_mono(mono);

    // Warm up caches and let filters settle before timing.
    for (int i = 0; i < 16; ++i) {
      processor.process(input.data(), output.data(), block);
    }

    bool finite = true;
    float peak = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) {
      processor.process(input.data(), output.data(), block);
      const jsfx::AudioFrame& probe = output[static_cast<std::size_t>(i % block)];
      if (!std::isfinite(probe.left) || !std::isfinite(probe.right)) {
        finite = false;
      }
      peak = std::max({peak, std::fabs(probe.left), std::fabs(probe.right)});
    }
    const auto end = std::chrono::steady_clock::now();

    const double create_ms =
        std::chrono::duration<double, std::milli>(create_end - create_start)
            .count();
    const double total_us =
        std::chrono::duration<double, std::micro>(end - start).count();
    const double us_per_block = total_us / blocks;
    const double block_budget_us = 1e6 * block / rate;
    const double realtime_pct = 100.0 * us_per_block / block_budget_us;

    std::printf("%s\t%s\t%.2f\t%.2f\t%.3f\t%.3f\n",
                finite ? "OK" : "NONFINITE", path, create_ms, us_per_block,
                realtime_pct, peak);
    std::fflush(stdout);
    if (!finite) ++failures;
  }
  return failures == 0 ? 0 : 1;
}
