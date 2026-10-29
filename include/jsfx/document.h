#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsfx/result.h"
#include "jsfx/slider.h"

namespace jsfx {

enum class JsfxSectionKind {
  Init = 0,
  Slider,
  Block,
  Sample,
  Count,
};

/// One executable section (`@init`, `@slider`, `@block`, `@sample`) extracted
/// from a JSFX file. `@gfx` and `@serialize` are stripped and never appear here.
struct JsfxSection {
  bool present = false;
  std::string code;

  /// 1-based line of the `@section` header in the original file.
  int header_line = 0;

  /// 1-based line of the first code line (the line after the header).
  int first_code_line = 0;

  /// EEL2 expects the number of lines preceding the chunk. Because `code`
  /// starts on the line after the header, this is the header line.
  int eel_line_offset() const { return header_line; }
};

/// `@init` code pulled in through an `import` line. Imported libraries are
/// compiled before the importing file so their functions are visible to it.
struct JsfxImport {
  std::string name;
  std::string init_code;
};

/// Returns the text of an imported file, or nullopt if it cannot be found.
using JsfxImportResolver =
    std::function<std::optional<std::string>(std::string_view name)>;

/// A non-fatal parser observation, e.g. an unknown section that was stripped.
struct JsfxWarning {
  std::string message;
  int line = 0;
};

/// Parsed form of a `.jsfx` file.
///
/// Parsing is a line-oriented state machine and runs on the main thread.
/// Comments and metadata before the first section are retained only insofar as
/// sliders are extracted; the original source is not stored.
class JsfxDocument {
 public:
  static constexpr int kMaxSliders = 256;
  static constexpr int kMaxImportDepth = 16;

  /// Parse JSFX source text. A UTF-8 BOM and CRLF line endings are accepted.
  /// `source_name` is advisory and used in diagnostics. `import` lines are
  /// resolved through `resolver`; without one they produce a warning.
  ///
  /// Like REAPER, the parser is lenient about slider declarations: a line it
  /// cannot make sense of becomes a warning instead of failing the file.
  static Result<JsfxDocument> parse(std::string_view source,
                                    std::string source_name = {},
                                    const JsfxImportResolver& resolver = {});

  const std::vector<JsfxSlider>& sliders() const { return sliders_; }

  /// Returns the slider with the given 1-based index, or nullptr.
  const JsfxSlider* find_slider(int index) const noexcept;

  const JsfxSection& section(JsfxSectionKind kind) const;
  bool has_section(JsfxSectionKind kind) const;

  /// Imported libraries in dependency order (deepest first).
  const std::vector<JsfxImport>& imports() const { return imports_; }

  const std::vector<JsfxWarning>& warnings() const { return warnings_; }
  const std::string& source_name() const { return source_name_; }

 private:
  std::array<JsfxSection, static_cast<std::size_t>(JsfxSectionKind::Count)>
      sections_{};
  std::vector<JsfxSlider> sliders_;
  std::vector<JsfxImport> imports_;
  std::vector<JsfxWarning> warnings_;
  std::string source_name_;

  struct ParseState;
  static Result<bool> parse_into(JsfxDocument& document, std::string_view source,
                                 const JsfxImportResolver& resolver,
                                 ParseState& state, bool is_import,
                                 std::string* import_init);
};

const char* to_string(JsfxSectionKind kind) noexcept;

}  // namespace jsfx
