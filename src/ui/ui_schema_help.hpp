// MIT License © 2025 Binary Dice Games
/// @file ui_schema_help.hpp
/// @brief Server-only queries over the registered "wish" UI element class
///        registry, plus a JSON-cursor-position scanner -- the shared basis
///        for the editor module's help panel and autocompletion.
///
/// Like `ui_importer.hpp`, this depends on bison's class registry
/// (`bison::dynamic::getRegistry()`), which is only populated server-side by
/// `register_all()` (see `src/server/registry.cpp`), so this header is only
/// compiled into `wish_server`.
#pragma once

#include "src/bison/bison_object.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::wish {

/// @brief One field of a registered UI element class, resolved for display
/// and autocomplete purposes.
///
/// `name` is the field's literal registration string (e.g. `"file_path"`),
/// recovered via `bison::lookup_registered_key_name()` -- this requires the
/// field to have been registered with the `_rkey` literal, not `_key` (see
/// `src/ui/ui_elements/*.cpp`'s `addField` call sites).
struct element_field_info {
  std::string name;
  std::string display_name;
  std::string description;
  std::string category;
  bool required = false;
  std::optional<std::pair<double, double>> range;
  std::optional<double> step;
  /// Valid value names from an `Enum`/`EnumFlags` attribute, if the field has
  /// one; empty otherwise.
  std::vector<std::string> enum_values;
  /// True if `enum_values` came from `EnumFlags` (OR-combinable bits) rather
  /// than `Enum` (a single exclusive value).
  bool is_enum_flags = false;
};

/// @brief A registered UI element class ("wish"-namespace, `Element`-derived),
/// resolved for display and autocomplete purposes.
///
/// `fields` includes both this class's own fields and every field inherited
/// from its `PARENT` chain up through (and including) `Element` itself
/// (`visible`, `children`, `order`, ...), most-derived definition winning on
/// a name collision.
struct element_class_info {
  std::string name;
  std::string display_name;
  std::string description;
  std::vector<element_field_info> fields;
};

/// @brief Enumerate every registered "wish"-namespace class whose `PARENT`
/// chain reaches `"Element"_key` -- i.e. every real embeddable UI element
/// type (`Button`, `Window`, `TextEditor`, every plot type, ...), excluding
/// `Element` itself and excluding `form` subclasses (`Editor`, `MessageBox`,
/// `FileDialog`, ... -- whole top-level RMI objects, never a JSON `"type"`
/// value), whose `PARENT` is `key_t{0U}` directly.
///
/// A class or field whose name was never `_rkey`-registered is silently
/// omitted (rather than fabricated from a `DisplayName`, which can mismatch
/// the literal spelling -- see `bison::lookup_registered_key_name()`'s doc
/// comment) -- this makes a missed `_key`->`_rkey` conversion visibly absent
/// from the results instead of silently wrong.
std::vector<element_class_info> enumerate_ui_element_classes();

/// @brief Look up one registered UI element class by its literal type name
/// (e.g. `"Window"`, as it would appear in a JSON `"type"` value).
/// @return The class's info, or `std::nullopt` if @p type_name is not a
/// registered "wish"-namespace class, or is not `Element`-derived.
std::optional<element_class_info> find_ui_element_class(const std::string& type_name);

/// @brief A cursor position within JSON source text: 0-based line, and
/// 0-based column counted in characters (codepoints), not bytes or visible
/// columns -- matches `ImGuiColorTextEdit::TextEditor::CursorPosition` and
/// `AutoCompleteState::searchTermStartIndex`'s own units, so callers need no
/// conversion. (ASCII-only class/field/type names are all this is ever
/// matched against, so multi-byte UTF-8 content elsewhere on the same line
/// cannot affect the result of `scan_cursor_context()`.)
struct text_pos {
  size_t line = 0;
  size_t column = 0;
};

/// @brief What kind of JSON token the cursor sits in or is about to start,
/// as determined by `scan_cursor_context()`.
enum class cursor_context_kind {
  /// No recognized JSON structure around the cursor (e.g. top-level, right
  /// after a closing brace, or the enclosing object's `"type"` hasn't been
  /// written yet -- see `scan_cursor_context()`'s doc comment).
  unknown,
  /// The cursor is inside (or about to start) the string value of the
  /// current object's own `"type"` field.
  type_value,
  /// The cursor is inside (or about to start) a field-name string, i.e. a
  /// JSON object key position.
  field_key,
  /// The cursor is inside (or about to start) a field's value, following
  /// some `"field_name": ` other than `"type"`.
  field_value,
};

/// @brief Result of scanning JSON source text up to a cursor position, to
/// answer "what wish UI schema element is the cursor editing right now."
struct cursor_context {
  cursor_context_kind kind = cursor_context_kind::unknown;
  /// The nearest enclosing object's own "type" value, if it has already
  /// been written textually before the cursor (see the "known limitation"
  /// in `scan_cursor_context()`'s doc comment for when this is empty even
  /// though the object does have a type).
  std::string enclosing_type;
  /// kind == field_value: which field this value belongs to.
  std::string field_name;
  /// Text already typed in the current in-progress token, for prefix
  /// filtering suggestions against. Empty if the cursor sits between
  /// tokens (nothing typed yet).
  std::string partial_text;
  /// kind == field_key: field names already present as siblings in the
  /// enclosing object, so they can be excluded from suggestions (the
  /// in-progress key itself, if any, is never included here).
  std::vector<std::string> existing_field_names;
};

/// @brief Determine what JSON schema element @p cursor is editing within
/// @p source, without requiring @p source to be valid JSON.
///
/// This is a hand-rolled, single-pass, **backward-only** scanner: it reads
/// only `source[0, offset)` (the byte offset corresponding to @p cursor) and
/// never looks past it. `import_json()`/nlohmann::json require the *entire*
/// document to parse, which makes them unusable here -- while actively
/// typing, the document is fully valid only in the rare instant right after
/// a complete edit (e.g. typing `"type": "But` mid-keystroke leaves an
/// unterminated string and unbalanced braces for every keystroke in
/// between, precisely when autocomplete needs to fire). Reading only up to
/// the cursor means a broken tail -- the normal state while mid-edit --
/// can never affect the result.
///
/// Known, accepted limitation: since the scan never looks ahead, positioning
/// the cursor *before* an object's own already-written `"type"` line (e.g.
/// on a blank line just after that object's opening `{`) yields an empty
/// `enclosing_type`, even though the object does have one a few lines further
/// down -- this degrades to `cursor_context_kind::unknown`/no suggestions in
/// that specific spot. Acceptable since every JSON example in this codebase
/// (including the editor module's own `kEditorLayout`) writes `"type"` first.
///
/// @param source  Raw JSON source text, valid or not.
/// @param cursor  Cursor position within @p source.
cursor_context scan_cursor_context(std::string_view source, text_pos cursor);

} // namespace bdg::wish
