#include "jsfx/document.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jsfx {
namespace {

struct SourceLine {
  std::string_view text;
  int number = 0;
};

bool is_blank(char c) { return c == ' ' || c == '\t'; }

char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

std::string_view trim_left(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  return text;
}

std::string_view trim_right(std::string_view text) {
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

std::string_view trim(std::string_view text) { return trim_right(trim_left(text)); }

std::vector<SourceLine> split_lines(std::string_view source) {
  // Editors commonly add a UTF-8 BOM; it is not part of the JSFX source.
  if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB &&
      static_cast<unsigned char>(source[2]) == 0xBF) {
    source.remove_prefix(3);
  }

  std::vector<SourceLine> lines;
  if (source.empty()) {
    lines.push_back({std::string_view{}, 1});
    return lines;
  }

  std::size_t pos = 0;
  int number = 1;
  while (pos < source.size()) {
    const std::size_t end = source.find_first_of("\r\n", pos);
    if (end == std::string_view::npos) {
      lines.push_back({source.substr(pos), number});
      break;
    }
    lines.push_back({source.substr(pos, end - pos), number});
    std::size_t newline_end = end;
    if (source[end] == '\r' && newline_end + 1 < source.size() &&
        source[newline_end + 1] == '\n') {
      ++newline_end;
    }
    pos = newline_end + 1;
    ++number;
  }
  return lines;
}

/// REAPER reads slider numbers with atof semantics: the longest numeric prefix
/// counts and anything unparseable is zero. Mirror that so sloppy headers
/// (`120.0.1`, `0.1}`) load the way they do in REAPER.
double lenient_number(std::string_view text) {
  std::string buffer(trim(text));
  char* end = nullptr;
  const double value = std::strtod(buffer.c_str(), &end);
  if (end == buffer.c_str() || !std::isfinite(value)) return 0.0;
  return value;
}

bool parse_number_prefix(std::string_view text, double& out, std::string_view& rest) {
  std::string buffer(text);
  char* end = nullptr;
  const double value = std::strtod(buffer.c_str(), &end);
  if (end == buffer.c_str() || !std::isfinite(value)) return false;
  out = value;
  rest = text.substr(static_cast<std::size_t>(end - buffer.c_str()));
  return true;
}

/// Split on commas that are not inside an enum label list `{...}`.
std::vector<std::string_view> split_top_level(std::string_view text) {
  std::vector<std::string_view> fields;
  int brace_depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++brace_depth;
    } else if (text[i] == '}' && brace_depth > 0) {
      --brace_depth;
    } else if (text[i] == ',' && brace_depth == 0) {
      fields.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  fields.push_back(text.substr(start));
  return fields;
}

/// End of a `<...>` range. Prefers a `>` outside `{labels}`, but an unclosed
/// label list (seen in shipping REAPER effects) ends at the first `>`.
std::size_t find_range_end(std::string_view text, std::size_t start) {
  int brace_depth = 0;
  for (std::size_t i = start; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++brace_depth;
    } else if (text[i] == '}' && brace_depth > 0) {
      --brace_depth;
    } else if (text[i] == '>' && brace_depth == 0) {
      return i;
    }
  }
  return text.find('>', start);
}

std::string sanitize_identifier(std::string_view text) {
  std::string result;
  bool separator_pending = false;
  for (char c : text) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    if (alnum) {
      if (separator_pending && !result.empty()) result.push_back('_');
      separator_pending = false;
      result.push_back(ascii_lower(c));
    } else {
      separator_pending = true;
    }
  }
  return result;
}

std::string make_property_name(int index, std::string_view description) {
  std::string name = "slider_" + std::to_string(index);
  const std::string suffix = sanitize_identifier(description);
  if (!suffix.empty()) {
    name.push_back('_');
    name += suffix;
  }
  return name;
}

bool is_identifier_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_identifier_char(char c) {
  return is_identifier_start(c) || is_digit(c) || c == '.';
}

bool looks_like_slider_line(std::string_view line) {
  line = trim_left(line);
  if (!starts_with_ci(line, "slider")) return false;
  const std::size_t pos = 6;  // strlen("slider")
  return pos < line.size() && is_digit(line[pos]);
}

/// Parse `<min,max[,step[:shape]][{labels}]>` contents into `slider`.
void parse_slider_range(std::string_view inner, JsfxSlider& slider) {
  const std::vector<std::string_view> fields = split_top_level(inner);
  slider.min_value = lenient_number(fields[0]);
  slider.max_value = fields.size() > 1 ? lenient_number(fields[1]) : 0.0;

  // Labels may sit on the step field or, in some files, a field of their own.
  std::string_view step_field = fields.size() > 2 ? fields[2] : std::string_view{};
  for (std::size_t i = 2; i < fields.size(); ++i) {
    const std::size_t brace = fields[i].find('{');
    if (brace == std::string_view::npos) continue;
    std::string_view labels = fields[i].substr(brace + 1);
    const std::size_t close = labels.find('}');
    if (close != std::string_view::npos) labels = labels.substr(0, close);
    for (std::string_view label : split_top_level(labels)) {
      slider.enum_labels.emplace_back(trim(label));
    }
    if (i == 2) step_field = fields[i].substr(0, brace);
    break;
  }

  // `0.1:log`, `0.1:log=1000`, `1:sqr=2` select REAPER's slider shapes.
  const std::size_t shape = step_field.find(':');
  if (shape != std::string_view::npos) {
    slider.logarithmic = starts_with_ci(trim(step_field.substr(shape + 1)), "log");
    step_field = step_field.substr(0, shape);
  }
  slider.step = std::fabs(lenient_number(step_field));
  if (slider.step == 0.0 && !slider.enum_labels.empty()) slider.step = 1.0;
  slider.has_range = true;
}

/// Returns a slider, or an error describing why the line was skipped.
Result<JsfxSlider> parse_slider_line(std::string_view line, int line_number) {
  line = trim_left(line);
  std::size_t pos = 6;  // caller verified the "slider" prefix

  long long index = 0;
  while (pos < line.size() && is_digit(line[pos])) {
    index = index * 10 + (line[pos] - '0');
    if (index > JsfxDocument::kMaxSliders) break;
    ++pos;
  }
  if (index < 1 || index > JsfxDocument::kMaxSliders) {
    return Result<JsfxSlider>::failure("slider index out of range (1..256)",
                                       line_number);
  }

  while (pos < line.size() && is_blank(line[pos])) ++pos;
  if (pos >= line.size() || line[pos] != ':') {
    return Result<JsfxSlider>::failure("expected ':' after slider index", line_number);
  }
  ++pos;

  JsfxSlider slider;
  slider.index = static_cast<int>(index);
  std::string_view rest = trim_left(line.substr(pos));

  if (!rest.empty() && rest.front() == '/') {
    // File slider: `/folder:default_file:description`.
    slider.is_file = true;
    const std::size_t first = rest.find(':');
    const std::size_t second =
        first == std::string_view::npos ? first : rest.find(':', first + 1);
    rest = second == std::string_view::npos ? std::string_view{}
                                            : rest.substr(second + 1);
  } else {
    // Named slider: `slider1:gain_db=0<...>`.
    if (!rest.empty() && is_identifier_start(rest.front())) {
      std::size_t end = 1;
      while (end < rest.size() && is_identifier_char(rest[end])) ++end;
      std::size_t eq = end;
      while (eq < rest.size() && is_blank(rest[eq])) ++eq;
      if (eq < rest.size() && rest[eq] == '=') {
        slider.variable_name = std::string(rest.substr(0, end));
        rest = trim_left(rest.substr(eq + 1));
      }
    }

    if (!parse_number_prefix(rest, slider.default_value, rest)) {
      slider.default_value = 0.0;
    }

    rest = trim_left(rest);
    if (!rest.empty() && rest.front() == '<') {
      const std::size_t close = find_range_end(rest, 1);
      if (close == std::string_view::npos) {
        parse_slider_range(rest.substr(1), slider);
        rest = {};
      } else {
        parse_slider_range(rest.substr(1, close - 1), slider);
        rest = rest.substr(close + 1);
      }
    } else if (!rest.empty() && rest.front() == '>') {
      // Tolerance for the `sliderN:default>description` spelling.
      rest.remove_prefix(1);
    }
  }

  rest = trim(rest);
  if (!rest.empty() && rest.front() == '-') {
    slider.hidden = true;
    rest = trim_left(rest.substr(1));
  }
  slider.description = std::string(rest);
  slider.property_name = make_property_name(
      slider.index, slider.description.empty()
                        ? std::string_view(slider.variable_name)
                        : std::string_view(slider.description));
  return Result<JsfxSlider>::success(std::move(slider));
}

/// `import foo.jsfx-inc` (case-insensitive keyword, trailing comment allowed).
bool parse_import_line(std::string_view line, std::string_view& name) {
  line = trim(line);
  if (!starts_with_ci(line, "import") || line.size() <= 6 || !is_blank(line[6])) {
    return false;
  }
  name = trim(line.substr(7));
  const std::size_t comment = name.find("//");
  if (comment != std::string_view::npos) name = trim(name.substr(0, comment));
  return !name.empty();
}

}  // namespace

const char* to_string(JsfxSectionKind kind) noexcept {
  switch (kind) {
    case JsfxSectionKind::Init:
      return "@init";
    case JsfxSectionKind::Slider:
      return "@slider";
    case JsfxSectionKind::Block:
      return "@block";
    case JsfxSectionKind::Sample:
      return "@sample";
    case JsfxSectionKind::Count:
      break;
  }
  return "";
}

struct JsfxDocument::ParseState {
  int depth = 0;
  std::vector<std::string> imported;
};

Result<JsfxDocument> JsfxDocument::parse(std::string_view source,
                                         std::string source_name,
                                         const JsfxImportResolver& resolver) {
  JsfxDocument document;
  document.source_name_ = std::move(source_name);

  ParseState state;
  Result<bool> parsed =
      parse_into(document, source, resolver, state, false, nullptr);
  if (!parsed) return Result<JsfxDocument>::failure(parsed.error());

  std::sort(document.sliders_.begin(), document.sliders_.end(),
            [](const JsfxSlider& a, const JsfxSlider& b) { return a.index < b.index; });
  return Result<JsfxDocument>::success(std::move(document));
}

Result<bool> JsfxDocument::parse_into(JsfxDocument& document,
                                      std::string_view source,
                                      const JsfxImportResolver& resolver,
                                      ParseState& parse_state, bool is_import,
                                      std::string* import_init) {
  const std::vector<SourceLine> lines = split_lines(source);

  enum class State { Header, InSection, Stripped };
  State state = State::Header;
  JsfxSectionKind current = JsfxSectionKind::Init;

  // Imported files contribute only @init (their function library). Their code
  // is collected separately so line numbers in the main file stay exact.
  std::array<JsfxSection, static_cast<std::size_t>(JsfxSectionKind::Count)>
      import_sections{};
  auto& sections = is_import ? import_sections : document.sections_;
  std::array<int, static_cast<std::size_t>(JsfxSectionKind::Count)> line_count{};
  std::array<bool, kMaxSliders + 1> slider_seen{};
  for (const JsfxSlider& slider : document.sliders_) slider_seen[slider.index] = true;

  for (const SourceLine& line : lines) {
    const std::string_view left = trim_left(line.text);

    if (!left.empty() && left.front() == '@') {
      std::size_t name_end = 1;
      while (name_end < left.size() && !is_blank(left[name_end])) ++name_end;
      const std::string_view name = left.substr(1, name_end - 1);

      JsfxSectionKind kind = JsfxSectionKind::Count;
      if (iequals(name, "init")) {
        kind = JsfxSectionKind::Init;
      } else if (iequals(name, "slider")) {
        kind = JsfxSectionKind::Slider;
      } else if (iequals(name, "block")) {
        kind = JsfxSectionKind::Block;
      } else if (iequals(name, "sample")) {
        kind = JsfxSectionKind::Sample;
      }

      if (kind != JsfxSectionKind::Count) {
        const auto index = static_cast<std::size_t>(kind);
        JsfxSection& section = sections[index];
        if (!section.present) {
          section.present = true;
          section.header_line = line.number;
          section.first_code_line = line.number + 1;
        } else {
          // REAPER accepts a repeated section and concatenates the code. Pad
          // with blank lines so EEL2's line numbers still match the file.
          while (section.header_line + line_count[index] < line.number) {
            section.code.push_back('\n');
            ++line_count[index];
          }
        }
        state = State::InSection;
        current = kind;
      } else if (iequals(name, "gfx") || iequals(name, "serialize")) {
        // Godot owns UI and serialization; never hand these blocks to EEL2.
        state = State::Stripped;
      } else {
        document.warnings_.push_back(
            {"unknown section '@" + std::string(name) + "' stripped", line.number});
        state = State::Stripped;
      }
      continue;
    }

    switch (state) {
      case State::Header: {
        std::string_view import_name;
        if (parse_import_line(line.text, import_name)) {
          const std::string key(import_name);
          if (std::find(parse_state.imported.begin(), parse_state.imported.end(),
                        key) != parse_state.imported.end()) {
            break;  // already imported (diamond or repeated import)
          }
          if (parse_state.depth >= kMaxImportDepth) {
            return Result<bool>::failure("import nesting is too deep", line.number);
          }
          std::optional<std::string> text;
          if (resolver) text = resolver(import_name);
          if (!text) {
            document.warnings_.push_back(
                {"import '" + key + "' not found", line.number});
            break;
          }
          parse_state.imported.push_back(key);
          ++parse_state.depth;
          std::string init_code;
          Result<bool> nested = parse_into(document, *text, resolver, parse_state,
                                           true, &init_code);
          --parse_state.depth;
          if (!nested) {
            return Result<bool>::failure(
                "in import '" + key + "': " + nested.error().message, line.number);
          }
          document.imports_.push_back({key, std::move(init_code)});
          break;
        }

        if (!is_import && looks_like_slider_line(line.text)) {
          Result<JsfxSlider> parsed = parse_slider_line(line.text, line.number);
          if (!parsed) {
            document.warnings_.push_back(
                {parsed.error().message + "; slider ignored", line.number});
            break;
          }
          if (slider_seen[parsed.value().index]) {
            document.warnings_.push_back(
                {"duplicate slider" + std::to_string(parsed.value().index) +
                     "; later declaration ignored",
                 line.number});
            break;
          }
          slider_seen[parsed.value().index] = true;
          document.sliders_.push_back(std::move(parsed.value()));
        }
        break;
      }

      case State::InSection: {
        const auto index = static_cast<std::size_t>(current);
        JsfxSection& section = sections[index];
        section.code.append(line.text.data(), line.text.size());
        section.code.push_back('\n');
        ++line_count[index];
        break;
      }

      case State::Stripped:
        break;
    }
  }

  if (is_import && import_init != nullptr) {
    *import_init = std::move(
        import_sections[static_cast<std::size_t>(JsfxSectionKind::Init)].code);
    for (JsfxSectionKind kind : {JsfxSectionKind::Slider, JsfxSectionKind::Block,
                                 JsfxSectionKind::Sample}) {
      if (import_sections[static_cast<std::size_t>(kind)].present) {
        document.warnings_.push_back(
            {std::string("imported ") + to_string(kind) + " code is ignored", 0});
      }
    }
  }
  return Result<bool>::success(true);
}
const JsfxSlider* JsfxDocument::find_slider(int index) const noexcept {
  for (const JsfxSlider& slider : sliders_) {
    if (slider.index == index) return &slider;
  }
  return nullptr;
}

const JsfxSection& JsfxDocument::section(JsfxSectionKind kind) const {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= sections_.size()) {
    static const JsfxSection empty;
    return empty;
  }
  return sections_[index];
}

bool JsfxDocument::has_section(JsfxSectionKind kind) const {
  return section(kind).present;
}

}  // namespace jsfx
