#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include "jsfx/document.h"
#include "jsfx/eel2_context.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

jsfx::JsfxEnvironment make_environment() {
  jsfx::JsfxEnvironment environment;
  environment.sample_rate = 48000.0;
  environment.tempo = 120.0;
  environment.play_state = 1;
  return environment;
}

}  // namespace

TEST_CASE("EEL2 context publishes JSFX environment variables", "[eel2]") {
  auto parsed = jsfx::JsfxDocument::parse(
      "@sample\n"
      "spl0 = srate / 1000;\n"
      "spl1 = samplesblock + tempo + play_state;\n");
  REQUIRE(parsed.ok());

  auto created = jsfx::Eel2Context::create(parsed.value(), make_environment());
  REQUIRE(created.ok());
  jsfx::Eel2Context& context = *created.value();

  context.run_block(64);
  context.run_sample();

  REQUIRE(context.variable("spl0") != nullptr);
  CHECK(*context.variable("spl0") == Approx(48.0));
  CHECK(*context.variable("spl1") == Approx(64.0 + 120.0 + 1.0));
  CHECK(*context.variable("srate") == Approx(48000.0));

  CHECK(context.has_section(jsfx::JsfxSectionKind::Sample));
  CHECK_FALSE(context.has_section(jsfx::JsfxSectionKind::Init));

  CHECK(context.set_tempo(90.0));
  CHECK(context.set_play_state(0));
  context.run_block(16);
  context.run_sample();
  CHECK(*context.variable("spl1") == Approx(16.0 + 90.0 + 0.0));

  CHECK(context.variable("does_not_exist") == nullptr);
  CHECK_FALSE(context.set_tempo(-1.0));
}

TEST_CASE("EEL2 context runs @init then the initial @slider", "[eel2]") {
  auto parsed = jsfx::JsfxDocument::parse(
      "slider1:0.25<0,1,0.01>Gain\n"
      "@init\n"
      "state = 1;\n"
      "@slider\n"
      "state = state + 1;\n"
      "@sample\n"
      "spl0 = state + slider1;\n");
  REQUIRE(parsed.ok());

  auto created = jsfx::Eel2Context::create(parsed.value(), make_environment());
  REQUIRE(created.ok());
  jsfx::Eel2Context& context = *created.value();

  // @init set state=1, the initial @slider made it 2.
  context.run_block(1);
  context.run_sample();
  CHECK(*context.variable("spl0") == Approx(2.25));

  CHECK(context.slider(1) == Approx(0.25));
  CHECK(context.set_slider(1, 0.5));
  CHECK(context.slider(1) == Approx(0.5));

  // set_slider writes the value but does not run @slider by itself.
  context.run_sample();
  CHECK(*context.variable("spl0") == Approx(2.5));

  context.run_slider();
  context.run_sample();
  CHECK(*context.variable("spl0") == Approx(3.5));

  CHECK_FALSE(context.set_slider(0, 1.0));
  CHECK_FALSE(context.set_slider(65, 1.0));
  CHECK_FALSE(context.set_slider(2, 1.0));
}

TEST_CASE("functions declared in @init are callable from @sample", "[eel2]") {
  auto parsed = jsfx::JsfxDocument::parse(
      "@init\n"
      "function twice(x) ( x * 2 );\n"
      "@sample\n"
      "spl0 = twice(spl0);\n");
  REQUIRE(parsed.ok());

  auto created = jsfx::Eel2Context::create(parsed.value(), make_environment());
  REQUIRE(created.ok());
  jsfx::Eel2Context& context = *created.value();

  *context.variable("spl0") = 3.0;
  context.run_block(1);
  context.run_sample();
  CHECK(*context.variable("spl0") == Approx(6.0));
}

TEST_CASE("@block observes samplesblock", "[eel2]") {  auto parsed = jsfx::JsfxDocument::parse(
      "@block\n"
      "block_value = samplesblock;\n"
      "@sample\n"
      "spl0 = block_value;\n");
  REQUIRE(parsed.ok());

  auto created = jsfx::Eel2Context::create(parsed.value(), make_environment());
  REQUIRE(created.ok());
  jsfx::Eel2Context& context = *created.value();

  context.run_block(128);
  context.run_sample();
  CHECK(*context.variable("spl0") == Approx(128.0));
}

TEST_CASE("EEL2 compile errors report the JSFX source line", "[eel2]") {
  const std::string source =
      "slider1:0<0,1,1>Gain\n"  // line 1
      "@init\n"                 // line 2
      "x = 1;\n"                // line 3
      "@slider\n"               // line 4
      "y = x;\n"                // line 5
      "@sample\n"               // line 6
      "spl0 = x + ;\n";         // line 7

  auto parsed = jsfx::JsfxDocument::parse(source);
  REQUIRE(parsed.ok());

  auto created = jsfx::Eel2Context::create(parsed.value(), make_environment());
  REQUIRE_FALSE(created.ok());
  CHECK(created.error().line == 7);
  CHECK_THAT(created.error().message, ContainsSubstring("@sample"));
  CHECK_THAT(created.error().message, ContainsSubstring("syntax error"));
}

TEST_CASE("EEL2 context rejects invalid environment values", "[eel2]") {
  auto parsed = jsfx::JsfxDocument::parse("@sample\nspl0 = 0;\n");
  REQUIRE(parsed.ok());

  jsfx::JsfxEnvironment environment = make_environment();
  environment.sample_rate = 0.0;
  CHECK_FALSE(jsfx::Eel2Context::create(parsed.value(), environment).ok());

  environment = make_environment();
  environment.tempo = 0.0;
  CHECK_FALSE(jsfx::Eel2Context::create(parsed.value(), environment).ok());
}
