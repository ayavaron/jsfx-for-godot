#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "jsfx/processor.h"

using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

jsfx::JsfxEnvironment make_environment() {
  jsfx::JsfxEnvironment environment;
  environment.sample_rate = 48000.0;
  environment.tempo = 120.0;
  environment.play_state = 1;
  return environment;
}

std::string gain_script() {
  return
      "slider1:1<0,1,0.001>Gain\n"
      "@init\n"
      "gain = 1;\n"
      "@slider\n"
      "gain = slider1;\n"
      "@sample\n"
      "spl0 *= gain;\n"
      "spl1 *= gain;\n";
}

void accumulate_squares(const jsfx::AudioFrame* frames, int count,
                        double& left, double& right) {
  for (int i = 0; i < count; ++i) {
    left += static_cast<double>(frames[i].left) * frames[i].left;
    right += static_cast<double>(frames[i].right) * frames[i].right;
  }
}

void fill_constant(jsfx::AudioFrame* frames, int count, float left, float right) {
  for (int i = 0; i < count; ++i) frames[i] = jsfx::AudioFrame{left, right};
}

}  // namespace

TEST_CASE("gain JSFX scales RMS by the slider value", "[processor][dsp]") {
  constexpr int kFrames = 48000;
  constexpr int kBlock = 480;

  auto created = jsfx::JsfxProcessor::create(gain_script(), make_environment(),
                                             "gain.jsfx");
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  REQUIRE(processor->set_slider(1, 0.5));
  CHECK(processor->get_slider(1) == Approx(0.5));

  std::vector<jsfx::AudioFrame> input(kBlock);
  std::vector<jsfx::AudioFrame> output(kBlock);

  double input_left = 0.0;
  double input_right = 0.0;
  double output_left = 0.0;
  double output_right = 0.0;

  for (int offset = 0; offset < kFrames; offset += kBlock) {
    const int count = std::min(kBlock, kFrames - offset);
    for (int i = 0; i < count; ++i) {
      const int sample = offset + i;
      input[i].left = static_cast<float>(
          0.25 * std::sin(kTwoPi * 1000.0 * sample / 48000.0));
      input[i].right = static_cast<float>(
          0.125 * std::sin(kTwoPi * 440.0 * sample / 48000.0));
    }

    processor->process(input.data(), output.data(), count);

    accumulate_squares(input.data(), count, input_left, input_right);
    accumulate_squares(output.data(), count, output_left, output_right);
  }

  const double in_l = std::sqrt(input_left / kFrames);
  const double in_r = std::sqrt(input_right / kFrames);
  const double out_l = std::sqrt(output_left / kFrames);
  const double out_r = std::sqrt(output_right / kFrames);

  // PRD Story 6.1: output RMS matches input RMS * gain within 1e-6.
  CHECK(out_l == Approx(in_l * 0.5).epsilon(1e-6));
  CHECK(out_r == Approx(in_r * 0.5).epsilon(1e-6));
  CHECK(processor->document().sliders().size() == 1);
  CHECK(processor->document().sliders()[0].property_name == "slider_1_gain");
}

TEST_CASE("slider updates apply at the start of the next block",
          "[processor]") {
  auto created = jsfx::JsfxProcessor::create(gain_script(), make_environment());
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  std::vector<jsfx::AudioFrame> input(4);
  std::vector<jsfx::AudioFrame> output(4);

  fill_constant(input.data(), 4, 1.0f, -1.0f);
  processor->process(input.data(), output.data(), 4);
  CHECK(output[0].left == Approx(1.0));
  CHECK(output[0].right == Approx(-1.0));

  REQUIRE(processor->set_slider(1, 0.5));
  CHECK(processor->get_slider(1) == Approx(0.5));
  fill_constant(input.data(), 4, 1.0f, -1.0f);
  processor->process(input.data(), output.data(), 4);
  CHECK(output[0].left == Approx(0.5));
  CHECK(output[0].right == Approx(-0.5));

  // In-place processing must also work.
  REQUIRE(processor->set_slider(1, 0.25));
  fill_constant(input.data(), 4, 1.0f, -1.0f);
  processor->process(input.data(), input.data(), 4);
  CHECK(input[0].left == Approx(0.25));
  CHECK(input[0].right == Approx(-0.25));

  CHECK_FALSE(processor->set_slider(0, 0.5));
  CHECK_FALSE(processor->set_slider(2, 0.5));
  CHECK_FALSE(processor->set_slider(
      1, std::numeric_limits<double>::quiet_NaN()));
  CHECK(processor->get_slider(2) == 0.0);
}

TEST_CASE("document without @sample passes audio through", "[processor]") {
  auto created = jsfx::JsfxProcessor::create(
      "@block\n"
      "block_value = samplesblock;\n",
      make_environment());
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  std::vector<jsfx::AudioFrame> input(8);
  std::vector<jsfx::AudioFrame> output(8);
  for (int i = 0; i < 8; ++i) {
    input[i] = jsfx::AudioFrame{0.1f * static_cast<float>(i),
                                -0.2f * static_cast<float>(i)};
  }

  processor->process(input.data(), output.data(), 8);
  for (int i = 0; i < 8; ++i) {
    CHECK(output[i].left == input[i].left);
    CHECK(output[i].right == input[i].right);
  }
}

TEST_CASE("@block and @sample see the current samplesblock", "[processor]") {
  auto created = jsfx::JsfxProcessor::create(
      "@block\n"
      "block_value = samplesblock;\n"
      "@sample\n"
      "spl0 = block_value;\n"
      "spl1 = block_value * 2;\n",
      make_environment());
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  std::vector<jsfx::AudioFrame> input(16);
  std::vector<jsfx::AudioFrame> output(16);
  processor->process(input.data(), output.data(), 16);

  CHECK(output[0].left == Approx(16.0));
  CHECK(output[0].right == Approx(32.0));
}

TEST_CASE("downsample factor skips @sample calls and holds output",
          "[processor][perf]") {
  const std::string source =
      "@sample\n"
      "counter += 1;\n"
      "spl0 = counter;\n"
      "spl1 = counter;\n";

  auto created = jsfx::JsfxProcessor::create(source, make_environment());
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  processor->set_downsample_factor(2);
  CHECK(processor->downsample_factor() == 2);

  std::vector<jsfx::AudioFrame> input(5);
  std::vector<jsfx::AudioFrame> output(5);

  processor->process(input.data(), output.data(), 5);
  CHECK(output[0].left == Approx(1.0));
  CHECK(output[1].left == Approx(1.0));
  CHECK(output[2].left == Approx(2.0));
  CHECK(output[3].left == Approx(2.0));
  CHECK(output[4].left == Approx(3.0));
  CHECK(output[4].right == Approx(3.0));

  // The phase continues across process() calls (block boundaries).
  processor->process(input.data(), output.data(), 5);
  CHECK(output[0].left == Approx(3.0));
  CHECK(output[1].left == Approx(4.0));
  CHECK(output[2].left == Approx(4.0));
  CHECK(output[3].left == Approx(5.0));
  CHECK(output[4].left == Approx(5.0));

  auto created_factor4 = jsfx::JsfxProcessor::create(source, make_environment());
  REQUIRE(created_factor4.ok());
  auto factor4 = std::move(created_factor4.value());
  factor4->set_downsample_factor(4);
  std::vector<jsfx::AudioFrame> input6(6);
  std::vector<jsfx::AudioFrame> output6(6);
  factor4->process(input6.data(), output6.data(), 6);
  CHECK(output6[0].left == Approx(1.0));
  CHECK(output6[1].left == Approx(1.0));
  CHECK(output6[2].left == Approx(1.0));
  CHECK(output6[3].left == Approx(1.0));
  CHECK(output6[4].left == Approx(2.0));
  CHECK(output6[5].left == Approx(2.0));

  factor4->set_downsample_factor(0);
  CHECK(factor4->downsample_factor() == 1);
  factor4->set_downsample_factor(100);
  CHECK(factor4->downsample_factor() ==
        jsfx::JsfxProcessor::kMaxDownsampleFactor);
}

TEST_CASE("force_mono feeds only the left input and duplicates spl0",
          "[processor][perf]") {
  const std::string source =
      "@sample\n"
      "spl0 = spl0 + spl1;\n"
      "spl1 = 123;\n";

  SECTION("stereo") {
    auto created = jsfx::JsfxProcessor::create(source, make_environment());
    REQUIRE(created.ok());
    auto processor = std::move(created.value());
    CHECK_FALSE(processor->force_mono());

    jsfx::AudioFrame input{1.0f, 100.0f};
    jsfx::AudioFrame output{};
    processor->process(&input, &output, 1);
    CHECK(output.left == Approx(101.0));
    CHECK(output.right == Approx(123.0));
  }

  SECTION("mono") {
    auto created = jsfx::JsfxProcessor::create(source, make_environment());
    REQUIRE(created.ok());
    auto processor = std::move(created.value());
    processor->set_force_mono(true);
    CHECK(processor->force_mono());

    jsfx::AudioFrame input{1.0f, 100.0f};
    jsfx::AudioFrame output{};
    processor->process(&input, &output, 1);
    // spl1 never receives the 100 right input, and spl0 is copied to both.
    CHECK(output.left == Approx(1.0));
    CHECK(output.right == Approx(1.0));
  }
}

TEST_CASE("EEL2 memory accesses around the 8M slot boundary do not crash",
          "[processor][memory]") {  // 8000001 is inside the block-rounded 8 Mi-slot window. 8388608 is the first
  // slot outside it, and 134217728 is outside EEL2's entire block table. Both
  // must resolve to EEL2's shared failure slot rather than fault.
  const std::string source =
      "@sample\n"
      "idx = 8000001;\n"
      "idx[0] = 1.0;\n"
      "a = idx[0];\n"
      "edge = 8388607;\n"
      "edge[0] = 3.0;\n"
      "b = edge[0];\n"
      "past = 8388608;\n"
      "past[0] = 2.0;\n"
      "c = past[0];\n"
      "huge = 134217728;\n"
      "huge[0] = 4.0;\n"
      "d = huge[0];\n"
      "spl0 = a + b;\n"
      "spl1 = c + d;\n";

  auto created = jsfx::JsfxProcessor::create(source, make_environment());
  REQUIRE(created.ok());
  auto processor = std::move(created.value());

  jsfx::AudioFrame input{0.0f, 0.0f};
  jsfx::AudioFrame output{};
  processor->process(&input, &output, 1);

  CHECK(output.left == Approx(4.0));
  CHECK(std::isfinite(output.left));
  CHECK(std::isfinite(output.right));
}

namespace {

jsfx::AudioFrame process_one(jsfx::JsfxProcessor& processor, float left,
                             float right) {
  jsfx::AudioFrame frame{left, right};
  processor.process(&frame, &frame, 1);
  return frame;
}

}  // namespace

TEST_CASE("processor provides JSFX host functions", "[processor]") {
  SECTION("slider() and spl() are lvalue references") {
    auto created = jsfx::JsfxProcessor::create(
        "slider1:0.5<0,1,0.01>A\n"
        "slider2:0.25<0,1,0.01>B\n"
        "@init\n"
        "slider(2) = 0.75;\n"
        "@sample\n"
        "spl(1) = slider(1) + slider(2);\n"
        "spl(0) = spl(1) * 2;\n"
        "spl(99) = 5; x = slider(500);\n",
        make_environment());
    REQUIRE(created.ok());
    const jsfx::AudioFrame out = process_one(*created.value(), 0.0f, 0.0f);
    CHECK(out.right == Approx(1.25));
    CHECK(out.left == Approx(2.5));
    CHECK(created.value()->get_slider(2) == Approx(0.75));  // @init wrote it
  }

  SECTION("named sliders store values in their variable") {
    auto created = jsfx::JsfxProcessor::create(
        "slider1:gain=0.5<0,1,0.01>Gain\n"
        "@sample\n"
        "spl0 *= gain;\n"
        "spl1 *= slider(1);\n",
        make_environment());
    REQUIRE(created.ok());
    jsfx::JsfxProcessor& processor = *created.value();
    CHECK(process_one(processor, 1.0f, 1.0f).left == Approx(0.5));
    REQUIRE(processor.set_slider(1, 0.25));
    const jsfx::AudioFrame out = process_one(processor, 1.0f, 1.0f);
    CHECK(out.left == Approx(0.25));
    CHECK(out.right == Approx(0.25));
  }

  SECTION("REAPER-only APIs compile as harmless no-ops") {
    auto created = jsfx::JsfxProcessor::create(
        "slider1:0<0,1,1>A\n"
        "@block\n"
        "while (midirecv(ofs, m1, m2)) ( midisend(ofs, m1, m2); );\n"
        "sliderchange(slider1); slider_automate(slider1);\n"
        "next = slider_next_chg(1, v);\n"
        "chans = get_host_numchan();\n"
        "t = time_precise();\n"
        "@sample\n"
        "spl0 = next + chans + (t > 0) + num_ch;\n",
        make_environment());
    REQUIRE(created.ok());
    CHECK(process_one(*created.value(), 0.0f, 0.0f).left == Approx(-1 + 2 + 1 + 2));
  }

  SECTION("fft round-trips a buffer") {
    auto created = jsfx::JsfxProcessor::create(
        "@init\n"
        "n = 16; i = 0;\n"
        "loop(n, buf[i*2] = sin(i); buf[i*2+1] = 0; i += 1);\n"
        "fft(buf, n); ifft(buf, n);\n"
        "@sample\n"
        "spl0 = buf[6] / n;\n",
        make_environment());
    REQUIRE(created.ok());
    CHECK(process_one(*created.value(), 0.0f, 0.0f).left ==
          Approx(std::sin(3.0)).margin(1e-9));
  }

  SECTION("transport position advances while playing") {
    auto created = jsfx::JsfxProcessor::create(
        "@block\n"
        "pos = beat_position;\n"
        "@sample\n"
        "spl0 = pos;\n",
        make_environment());
    REQUIRE(created.ok());
    std::vector<jsfx::AudioFrame> block(24000);
    created.value()->process(block.data(), block.data(), 24000);
    created.value()->process(block.data(), block.data(), 24000);
    // Half a second at 120 BPM is one beat.
    CHECK(block[0].left == Approx(1.0));
  }
}

TEST_CASE("processor runs imported libraries", "[processor]") {
  const jsfx::JsfxImportResolver resolver =
      [](std::string_view name) -> std::optional<std::string> {
    if (name != "lib.jsfx-inc") return std::nullopt;
    return std::string("@init\nlib_scale = 3;\nfunction scale(x) (x * lib_scale);\n");
  };
  auto parsed = jsfx::JsfxDocument::parse(
      "import lib.jsfx-inc\n@sample\nspl0 = scale(spl0);\n", "main.jsfx",
      resolver);
  REQUIRE(parsed.ok());
  auto created =
      jsfx::JsfxProcessor::create(std::move(parsed.value()), make_environment());
  REQUIRE(created.ok());
  CHECK(process_one(*created.value(), 0.5f, 0.0f).left == Approx(1.5));
}

TEST_CASE("processor flushes denormals", "[processor]") {
  // A decaying one-pole filter reaches the subnormal range quickly; with
  // flush-to-zero the state collapses to exactly zero instead.
  auto created = jsfx::JsfxProcessor::create(
      "@sample\n"
      "state = state * 0.5 + spl0;\n"
      "spl0 = state;\n",
      make_environment());
  REQUIRE(created.ok());
  std::vector<jsfx::AudioFrame> block(4096);
  block[0].left = 1.0f;
  created.value()->process(block.data(), block.data(), 4096);
  CHECK(block[4095].left == 0.0f);
  CHECK(std::fpclassify(block[1100].left) != FP_SUBNORMAL);
}
