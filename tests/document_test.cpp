#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <catch2/catch_approx.hpp>

#include <optional>
#include <string>
#include <vector>

#include "jsfx/document.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("document extracts sections and slider metadata", "[document]") {
  const std::string source =
      "desc: Test Gain\n"
      "slider1:0.5<0,1,0.001>Gain\n"
      "slider2:0<-60,12,0.1>Trim (dB)\n"
      "// a comment\n"
      "@init\n"
      "gain = slider1;\n"
      "@slider\n"
      "gain = slider1;\n"
      "@block\n"
      "@sample\n"
      "spl0 *= gain;\n"
      "spl1 *= gain;\n"
      "@gfx 640 480\n"
      "gfx_should_be_stripped = 1;\n"
      "@serialize\n"
      "serialize_should_be_stripped = 1;\n";

  auto parsed = jsfx::JsfxDocument::parse(source, "gain.jsfx");
  REQUIRE(parsed.ok());
  const jsfx::JsfxDocument& document = parsed.value();

  CHECK(document.source_name() == "gain.jsfx");
  CHECK(document.warnings().empty());

  REQUIRE(document.sliders().size() == 2);

  const jsfx::JsfxSlider& gain = document.sliders()[0];
  CHECK(gain.index == 1);
  CHECK(gain.default_value == 0.5);
  CHECK(gain.has_range);
  CHECK(gain.min_value == 0.0);
  CHECK(gain.max_value == 1.0);
  CHECK(gain.step == 0.001);
  CHECK(gain.description == "Gain");
  CHECK(gain.property_name == "slider_1_gain");
  CHECK(gain.enum_labels.empty());

  const jsfx::JsfxSlider& trim = document.sliders()[1];
  CHECK(trim.index == 2);
  CHECK(trim.description == "Trim (dB)");
  CHECK(trim.property_name == "slider_2_trim_db");

  const jsfx::JsfxSection& init = document.section(jsfx::JsfxSectionKind::Init);
  CHECK(init.present);
  CHECK(init.header_line == 5);
  CHECK(init.first_code_line == 6);
  CHECK(init.code == "gain = slider1;\n");

  const jsfx::JsfxSection& slider =
      document.section(jsfx::JsfxSectionKind::Slider);
  CHECK(slider.present);
  CHECK(slider.header_line == 7);
  CHECK(slider.code == "gain = slider1;\n");

  const jsfx::JsfxSection& block = document.section(jsfx::JsfxSectionKind::Block);
  CHECK(block.present);
  CHECK(block.header_line == 9);
  CHECK(block.code.empty());

  const jsfx::JsfxSection& sample =
      document.section(jsfx::JsfxSectionKind::Sample);
  CHECK(sample.present);
  CHECK(sample.header_line == 10);
  CHECK_THAT(sample.code, ContainsSubstring("spl0 *= gain;"));
  CHECK_THAT(sample.code, ContainsSubstring("spl1 *= gain;"));

  for (jsfx::JsfxSectionKind kind :
       {jsfx::JsfxSectionKind::Init, jsfx::JsfxSectionKind::Slider,
        jsfx::JsfxSectionKind::Block, jsfx::JsfxSectionKind::Sample}) {
    CHECK_THAT(document.section(kind).code,
               !ContainsSubstring("gfx_should_be_stripped"));
    CHECK_THAT(document.section(kind).code,
               !ContainsSubstring("serialize_should_be_stripped"));
  }
}

TEST_CASE("document accepts CRLF, a BOM, enums and range-less sliders",
          "[document]") {
  const std::string source =
      "\xEF\xBB\xBF"
      "slider1:0<0,3,1{Low,Medium,High}>Mode\r\n"
      "slider2:0.75>Wet\r\n"
      "slider3:-6<-60,12>Level\r\n"
      "@sample\r\n"
      "spl0 = slider1;\r\n";

  auto parsed = jsfx::JsfxDocument::parse(source);
  REQUIRE(parsed.ok());
  const jsfx::JsfxDocument& document = parsed.value();
  REQUIRE(document.sliders().size() == 3);

  const jsfx::JsfxSlider& mode = document.sliders()[0];
  CHECK(mode.index == 1);
  CHECK(mode.has_range);
  CHECK(mode.min_value == 0.0);
  CHECK(mode.max_value == 3.0);
  CHECK(mode.step == 1.0);
  CHECK(mode.description == "Mode");
  CHECK(mode.property_name == "slider_1_mode");
  CHECK(mode.enum_labels == std::vector<std::string>{"Low", "Medium", "High"});

  const jsfx::JsfxSlider& wet = document.sliders()[1];
  CHECK_FALSE(wet.has_range);
  CHECK(wet.description == "Wet");
  CHECK(wet.property_name == "slider_2_wet");

  const jsfx::JsfxSlider& level = document.sliders()[2];
  CHECK(level.min_value == -60.0);
  CHECK(level.max_value == 12.0);
  CHECK(level.step == 0.0);

  CHECK(document.section(jsfx::JsfxSectionKind::Sample).code ==
        "spl0 = slider1;\n");
  CHECK_FALSE(document.has_section(jsfx::JsfxSectionKind::Init));
  CHECK_FALSE(document.has_section(jsfx::JsfxSectionKind::Block));
}

TEST_CASE("document sanitizes property names", "[document]") {
  const std::string source =
      "slider1:0<0,1,1>\n"
      "slider2:0<0,1,1>Trim (dB)\n"
      "slider3:0<0,1,1>  2x Gain!!\n"
      "slider4:0<0,1,1>---\n";

  auto parsed = jsfx::JsfxDocument::parse(source);
  REQUIRE(parsed.ok());
  const auto& sliders = parsed.value().sliders();
  REQUIRE(sliders.size() == 4);
  CHECK(sliders[0].property_name == "slider_1");
  CHECK(sliders[1].property_name == "slider_2_trim_db");
  CHECK(sliders[2].property_name == "slider_3_2x_gain");
  CHECK(sliders[3].property_name == "slider_4");
}

TEST_CASE("unknown sections are stripped with a warning", "[document]") {
  const std::string source =
      "slider1:0<0,1,1>Gain\n"
      "@init\n"
      "x = 1;\n"
      "@custom\n"
      "y = 2;\n"
      "@sample\n"
      "spl0 = x;\n";

  auto parsed = jsfx::JsfxDocument::parse(source);
  REQUIRE(parsed.ok());
  const jsfx::JsfxDocument& document = parsed.value();

  REQUIRE(document.warnings().size() == 1);
  CHECK(document.warnings()[0].line == 4);
  CHECK_THAT(document.warnings()[0].message, ContainsSubstring("@custom"));
  CHECK(document.section(jsfx::JsfxSectionKind::Init).code == "x = 1;\n");
  CHECK_THAT(document.section(jsfx::JsfxSectionKind::Sample).code,
             !ContainsSubstring("y = 2;"));
}

TEST_CASE("document is as lenient as REAPER about slider headers", "[document]") {
  SECTION("repeated sections are concatenated with exact line numbers") {
    auto parsed = jsfx::JsfxDocument::parse(
        "@sample\n"
        "a = 1;\n"
        "@init\n"
        "b = 2;\n"
        "@sample\n"
        "c = 3;\n");
    REQUIRE(parsed.ok());
    const auto& sample = parsed.value().section(jsfx::JsfxSectionKind::Sample);
    // Line k of the code (0-based) is file line header_line + 1 + k.
    CHECK(sample.code == "a = 1;\n\n\n\nc = 3;\n");
  }

  SECTION("malformed sliders become warnings, not failures") {
    auto parsed = jsfx::JsfxDocument::parse(
        "slider1:0<0,1,1>A\n"
        "slider1:0<0,1,1>B\n"
        "slider300:0<0,1,1>Too high\n"
        "slider2 0<0,1,1>No colon\n");
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().sliders().size() == 1);
    CHECK(parsed.value().sliders()[0].description == "A");
    CHECK(parsed.value().warnings().size() == 3);
  }

  SECTION("sloppy numbers use atof semantics") {
    auto parsed = jsfx::JsfxDocument::parse(
        "slider1:10<10,120.0.1>HPF Freq\n"
        "slider2:0<0,10,0.1}>-frequency curve\n"
        "slider3:abc<0,1,1>No default\n"
        "slider4:0<0,1,1\n");
    REQUIRE(parsed.ok());
    const auto& s = parsed.value().sliders();
    REQUIRE(s.size() == 4);
    CHECK(s[0].max_value == 120.0);
    CHECK(s[1].step == Catch::Approx(0.1));
    CHECK(s[1].hidden);
    CHECK(s[1].description == "frequency curve");
    CHECK(s[2].default_value == 0.0);
    CHECK(s[3].has_range);
  }

  SECTION("enum labels in odd places and unclosed label lists") {
    auto parsed = jsfx::JsfxDocument::parse(
        "slider1:0<0,1,1,{Disabled,Enabled}>Flush\n"
        "slider2:0<0,2,1{Stopped,Recording,Playing>State\n");
    REQUIRE(parsed.ok());
    const auto& s = parsed.value().sliders();
    REQUIRE(s.size() == 2);
    CHECK(s[0].enum_labels == std::vector<std::string>{"Disabled", "Enabled"});
    CHECK(s[1].enum_labels.size() == 3);
    CHECK(s[1].description == "State");
  }

  SECTION("inverted ranges are kept") {
    auto parsed = jsfx::JsfxDocument::parse("slider1:0<2,1,1>Inverted\n");
    REQUIRE(parsed.ok());
    CHECK(parsed.value().sliders()[0].min_value == 2.0);
  }
}

TEST_CASE("document understands modern slider syntax", "[document]") {
  auto parsed = jsfx::JsfxDocument::parse(
      "slider1:gain_db=-6<-60,12,0.1>Gain (dB)\n"
      "slider2:freq=1000<20,20000,1:log=1000>-Cutoff\n"
      "slider3:/samples:kick.wav:Sample\n"
      "slider4:mode=0<0,2,1{A,B,C}>\n"
      "slider256:0<0,1,0.01>Last\n");
  REQUIRE(parsed.ok());
  const auto& s = parsed.value().sliders();
  REQUIRE(s.size() == 5);
  CHECK(s[0].variable_name == "gain_db");
  CHECK(s[0].default_value == -6.0);
  CHECK(s[0].property_name == "slider_1_gain_db");
  CHECK(s[1].logarithmic);
  CHECK(s[1].step == 1.0);
  CHECK(s[1].hidden);
  CHECK(s[2].is_file);
  CHECK(s[2].description == "Sample");
  CHECK(s[3].property_name == "slider_4_mode");
  CHECK(s[4].index == 256);
}

TEST_CASE("document resolves imports", "[document]") {
  int calls = 0;
  const jsfx::JsfxImportResolver resolver =
      [&calls](std::string_view name) -> std::optional<std::string> {
    ++calls;
    if (name == "lib.jsfx-inc") {
      return std::string("import base.jsfx-inc\n@init\nfunction twice(x) (x * 2);\n");
    }
    if (name == "base.jsfx-inc") {
      return std::string("@init\nfunction one() (1);\n");
    }
    return std::nullopt;
  };

  auto parsed = jsfx::JsfxDocument::parse(
      "import lib.jsfx-inc\n"
      "import lib.jsfx-inc // repeated\n"
      "import missing.jsfx-inc\n"
      "@sample\nspl0 = twice(one());\n",
      "main.jsfx", resolver);
  REQUIRE(parsed.ok());
  const auto& imports = parsed.value().imports();
  REQUIRE(imports.size() == 2);
  CHECK(imports[0].name == "base.jsfx-inc");  // dependencies first
  CHECK(imports[1].name == "lib.jsfx-inc");
  CHECK_THAT(imports[1].init_code, ContainsSubstring("twice"));
  CHECK(calls == 3);
  REQUIRE(parsed.value().warnings().size() == 1);
  CHECK_THAT(parsed.value().warnings()[0].message, ContainsSubstring("missing"));
}
