// MIT License © 2025 Binary Dice Games
/// @file imgui_text_editor_renderer.cpp
/// @brief ImGui render function for the TextEditor wish UI element.
#include "imgui_ui_renderer.hpp"

#include "src/bison/bison_object.hpp"

#include <context/file_service.hpp>
#include <context/style_service.hpp>
#include <ui/ui_schema_help.hpp>

#include <TextEditor.h>
#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

namespace bdg::wish {

using namespace bdg::bison;

// ── Per-widget state ──────────────────────────────────────────────────────────

namespace {

struct TextEditorState {
  TextEditor editor;
  std::string loaded_path; // full path last loaded into the editor
  std::string loaded_lang; // language key last applied
  std::optional<bool> applied_is_light; // TextEditor::Get{Light,Dark}Palette() last applied
  size_t last_undo_index{0};
  TextEditor::DocPos last_cursor{}; // only tracked when wish_ui_schema is true
  bool autocomplete_configured{false}; // whether SetAutoCompleteConfig has been applied
  bool decorator_configured{false}; // whether SetLineDecorator has been applied
  std::vector<int32_t> breakpoint_lines; // 1-based, refreshed from the field every frame
  int32_t current_line{0}; // 1-based, 0 = none; refreshed from the field every frame
};

std::unordered_map<uint32_t, TextEditorState>& editor_cache() {
  static std::unordered_map<uint32_t, TextEditorState> cache;
  return cache;
}

// ── YAML syntax highlighting ─────────────────────────────────────────────────
//
// ImGuiColorTextEdit (extern/, a third-party submodule wish does not modify)
// ships no YAML language definition, so wish assembles a lightweight one here
// from the public `TextEditor::Language` API. It covers what a wish UI
// descriptor actually uses: `#` line comments, single/double-quoted scalars,
// `true`/`false`/`null` and numeric scalars, `- ` sequence markers, and --
// via the custom tokenizer -- the `key:` of every mapping entry.

using TeIter = TextEditor::Iterator;

// A YAML mapping-key character. Unquoted keys in our descriptors are plain
// identifiers; also allow '-', '.', '/' and '$' so keys like "x_label" or
// "font-size" still colorize as one token. ('#' is deliberately excluded --
// it starts a comment.)
bool is_yaml_key_char(ImWchar c) {
  return TextEditor::CodePoint::isXidContinue(c) || c == '-' || c == '.' || c == '/' || c == '$';
}

// Custom tokenizer: colors a leading `key` (up to, not including, its `:`) as
// an identifier so mapping keys read differently from scalar values, and
// colors block-scalar / anchor / alias / tag indicators as punctuation.
// Everything else is deferred to getIdentifier/getNumber/isPunctuation.
TeIter tokenize_yaml(TeIter start, TeIter end, TextEditor::Color& color) {
  ImWchar first = start < end ? *start : 0;
  if (first == '|' || first == '>' || first == '&' || first == '*' || first == '!' || first == '%') {
    TeIter i = start;
    ++i;
    while (i < end && !TextEditor::CodePoint::isWhiteSpace(*i))
      ++i;
    color = TextEditor::Color::punctuation;
    return i;
  }

  if (!is_yaml_key_char(first))
    return start;

  TeIter key_end = start;
  while (key_end < end && is_yaml_key_char(*key_end))
    ++key_end;

  TeIter after = key_end;
  while (after < end && (*after == ' ' || *after == '\t'))
    ++after;
  if (after < end && *after == ':') {
    TeIter past_colon = after;
    ++past_colon;
    if (past_colon >= end || *past_colon == ' ' || *past_colon == '\t') {
      color = TextEditor::Color::identifier;
      return key_end; // the ':' itself is left for isPunctuation
    }
  }
  return start;
}

TeIter yaml_identifier(TeIter start, TeIter end) {
  TeIter i = start;
  if (i < end && (TextEditor::CodePoint::isXidStart(*i) || *i == '_')) {
    ++i;
    while (i < end && TextEditor::CodePoint::isXidContinue(*i))
      ++i;
  }
  return i;
}

TeIter yaml_number(TeIter start, TeIter end) {
  TeIter i = start;
  if (i < end && (*i == '-' || *i == '+'))
    ++i;
  bool any = false;
  while (i < end && (TextEditor::CodePoint::isNumber(*i) || *i == '.')) {
    any = true;
    ++i;
  }
  return any ? i : start;
}

bool yaml_punctuation(ImWchar c) {
  return c == ':' || c == '-' || c == '?' || c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

const TextEditor::Language* yaml_language() {
  static const TextEditor::Language language = [] {
    TextEditor::Language l;
    l.name = "YAML";
    l.singleLineComment = "#";
    l.hasSingleQuotedStrings = true;
    l.hasDoubleQuotedStrings = true;
    l.stringEscape = '\\';
    for (const char* kw : {"true", "false", "null", "yes", "no", "on", "off", "True", "False", "Null", "TRUE", "FALSE",
                           "NULL", "Yes", "No", "On", "Off", "~"})
      l.keywords.insert(kw);
    l.customTokenizer = tokenize_yaml;
    l.getIdentifier = yaml_identifier;
    l.getNumber = yaml_number;
    l.isPunctuation = yaml_punctuation;
    return l;
  }();
  return &language;
}

// Map wish language string → TextEditor::Language factory.
const TextEditor::Language* language_for(const std::string& lang) {
  if (lang == "yaml" || lang == "yml")
    return yaml_language();
  if (lang == "cpp" || lang == "c++")
    return TextEditor::Language::Cpp();
  if (lang == "c")
    return TextEditor::Language::C();
  if (lang == "cs")
    return TextEditor::Language::Cs();
  if (lang == "glsl")
    return TextEditor::Language::Glsl();
  if (lang == "hlsl")
    return TextEditor::Language::Hlsl();
  if (lang == "lua")
    return TextEditor::Language::Lua();
  if (lang == "python")
    return TextEditor::Language::Python();
  if (lang == "sql")
    return TextEditor::Language::Sql();
  if (lang == "json")
    return TextEditor::Language::Json();
  if (lang == "markdown")
    return TextEditor::Language::Markdown();
  if (lang == "angelscript")
    return TextEditor::Language::AngelScript();
  return nullptr;
}

// Fills `state.suggestions` for a wish-UI-JSON-schema-aware autocomplete
// popup, sourced from the live "wish" class registry (src/ui/ui_schema_help.hpp).
// `state.userData` is the TextEditor instance itself (set via
// AutoCompleteConfig::userData, see render_text_editor()), needed to read
// the full document text -- AutoCompleteState alone carries only the
// in-progress token's position, not the whole buffer.
void fill_wish_ui_schema_suggestions(TextEditor::AutoCompleteState& state) {
  // The library does not clear `suggestions` between callback invocations
  // (one per keystroke while typing) -- without this, matches from an
  // earlier, shorter prefix pile up alongside the current ones.
  state.suggestions.clear();

  auto* editor = static_cast<TextEditor*>(state.userData);
  if (!editor)
    return;

  // searchTermEnd, not searchTermStart: we want the current in-progress
  // cursor position (end of the token typed so far), not where the token
  // started -- using the start position makes scan_cursor_context see zero
  // characters typed yet, always yielding an empty partial_text (which
  // matches every candidate as a prefix, defeating filtering).
  text_pos pos{state.searchTermEnd.line, state.searchTermEnd.index};
  const auto* lang = editor->GetLanguage();
  bool is_yaml = lang && lang->name == "YAML";
  auto ctx = is_yaml ? scan_cursor_context_yaml(editor->GetText(), pos) : scan_cursor_context(editor->GetText(), pos);

  switch (ctx.kind) {
    case cursor_context_kind::type_value:
      for (const auto& c : enumerate_ui_element_classes()) {
        if (c.name.starts_with(ctx.partial_text))
          state.suggestions.push_back(c.name);
      }
      break;
    case cursor_context_kind::field_key: {
      auto found = find_ui_element_class(ctx.enclosing_type);
      if (!found)
        break;
      for (const auto& f : found->fields) {
        if (!f.name.starts_with(ctx.partial_text))
          continue;
        if (std::find(ctx.existing_field_names.begin(), ctx.existing_field_names.end(), f.name) !=
            ctx.existing_field_names.end())
          continue;
        state.suggestions.push_back(f.name);
      }
      break;
    }
    case cursor_context_kind::field_value: {
      auto found = find_ui_element_class(ctx.enclosing_type);
      if (!found)
        break;
      for (const auto& f : found->fields) {
        if (f.name != ctx.field_name)
          continue;
        for (const auto& v : f.enum_values) {
          if (v.starts_with(ctx.partial_text))
            state.suggestions.push_back(v);
        }
        break;
      }
      break;
    }
    case cursor_context_kind::unknown:
    default:
      break;
  }
}

} // namespace

// ── Render function ───────────────────────────────────────────────────────────

void render_text_editor(imgui_renderer&, const ui_element& node_base, const context& s) {
  const auto& node = static_cast<const ui_text_editor&>(node_base);
  const std::string& file_path = node.file_path_ref();
  auto language = node.language("none");
  auto read_only = node.read_only();
  auto wish_ui_schema = node.wish_ui_schema();
  int32_t w = node.width_i(0);
  int32_t h = node.height_i(400);

  if (file_path.empty())
    return;

  auto full_path = file_service::resolve_path(file_path, s.resource_dir, s.allow_absolute_paths);
  if (full_path.empty())
    return;

  auto id = node.wish_id();
  auto& st = editor_cache()[id.id];

  // Reload when the file path changes.
  if (st.loaded_path != full_path.string()) {
    st.loaded_path = full_path.string();
    std::ifstream f(full_path, std::ios::binary);
    st.editor.SetText(f ? std::string{std::istreambuf_iterator<char>(f), {}} : "");
    st.loaded_lang = language;
    st.editor.SetLanguage(language_for(language));
    st.last_undo_index = st.editor.GetUndoIndex();
  } else if (st.loaded_lang != language) {
    // Language changed without a file change — reapply highlighting.
    st.loaded_lang = language;
    st.editor.SetLanguage(language_for(language));
  }

  st.editor.SetReadOnlyEnabled(read_only);

  // wish UI JSON schema autocomplete: opt-in (see TextEditor.wish_ui_schema's
  // doc comment) so unrelated TextEditor uses (e.g. nano) are unaffected.
  // AutoCompleteConfig::setConfig() copies *cfg by value, so a stack local
  // is safe here -- it does not need to outlive this call.
  if (wish_ui_schema && (language == "json" || language == "yaml")) {
    if (!st.autocomplete_configured) {
      st.autocomplete_configured = true;
      TextEditor::AutoCompleteConfig cfg;
      cfg.triggerInStrings = true;
      cfg.userData = &st.editor;
      cfg.callback = fill_wish_ui_schema_suggestions;
      st.editor.SetAutoCompleteConfig(&cfg);
    }
  } else if (st.autocomplete_configured) {
    st.autocomplete_configured = false;
    st.editor.SetAutoCompleteConfig(nullptr);
  }

  // Sync the TextEditor palette with the session's active theme via
  // style_service::is_light_theme() -- the current preset's registered
  // is_light (see register_theme()'s doc comment), read live every frame
  // -- rather than matching preset names one by one here. TextEditor
  // maintains its own color palette independent of ImGuiStyle, so it needs
  // this explicit sync; render_label() (imgui_ui_renderer.cpp) reads the
  // very same flag to pick between a Label's text_color_light/
  // text_color_dark, so both follow the client's actual active theme
  // identically, including a theme change made after either widget's
  // content was created.
  {
    bool is_light = !s.style_service || s.style_service->is_light_theme();
    if (!st.applied_is_light || *st.applied_is_light != is_light) {
      st.applied_is_light = is_light;
      st.editor.SetPalette(is_light ? TextEditor::GetLightPalette() : TextEditor::GetDarkPalette());
    }
  }

  // Breakpoint/current-line gutter markers: refresh the cached, 1-based
  // snapshot every frame (cheap -- a handful of ints) so the decorator
  // callback below, registered once, always reads live state through `&st`.
  if (const auto* bps = node.breakpoint_lines())
    st.breakpoint_lines = *bps;
  else
    st.breakpoint_lines.clear();
  st.current_line = node.current_line();

  // Gutter decorator: a filled dot for each breakpoint line, or (taking
  // priority when a line is both) a filled triangle for the current
  // execution line -- mirrors how real debuggers show the active line's
  // marker over a breakpoint dot on the same line. ImGuiColorTextEdit
  // (vendored, never modified -- see DESIGN.md) exposes no line-background
  // highlight hook, so the arrow is the current-line indicator; width is 1
  // glyph, enough for either marker.
  if (!st.decorator_configured) {
    st.decorator_configured = true;
    st.editor.SetLineDecorator(1, [&st](TextEditor::Decorator& decorator) {
      int32_t line_1based = static_cast<int32_t>(decorator.line) + 1;
      auto* draw_list = ImGui::GetWindowDrawList();
      ImVec2 origin = ImGui::GetCursorScreenPos();
      ImVec2 center{origin.x + decorator.width * 0.5f, origin.y + decorator.height * 0.5f};
      float size = std::min(decorator.width, decorator.height) * 0.35f;

      if (line_1based == st.current_line) {
        ImU32 color = IM_COL32(230, 190, 40, 255);
        ImVec2 p1{center.x - size * 0.5f, center.y - size};
        ImVec2 p2{center.x - size * 0.5f, center.y + size};
        ImVec2 p3{center.x + size, center.y};
        draw_list->AddTriangleFilled(p1, p2, p3, color);
      } else if (std::find(st.breakpoint_lines.begin(), st.breakpoint_lines.end(), line_1based) !=
                 st.breakpoint_lines.end()) {
        ImU32 color = IM_COL32(200, 50, 50, 255);
        draw_list->AddCircleFilled(center, size, color);
      }
    });
  }

  // Right-click on a line number: "Toggle Breakpoint" menu item emitting
  // "line_context_menu" -- reassigned every frame (cheap std::function
  // move) so the closure captures this frame's `s`/`id`/`full_path` rather
  // than risking a dangling reference to a prior frame's context.
  st.editor.SetLineNumberContextMenuCallback([&s, id, &st](TextEditor::PopupData& data) {
    int32_t line_1based = static_cast<int32_t>(data.pos.line) + 1;
    bool has_breakpoint = std::find(st.breakpoint_lines.begin(), st.breakpoint_lines.end(), line_1based) !=
                          st.breakpoint_lines.end();
    if (ImGui::MenuItem(has_breakpoint ? "Remove Breakpoint" : "Add Breakpoint")) {
      dynamic payload;
      payload["line"_key] = line_1based;
      payload["has_breakpoint"_key] = has_breakpoint;
      enqueue_event(s, id, "line_context_menu"_key, std::move(payload));
    }
  });

  // "##te" alone is enough: render_node() (imgui_renderer.cpp) already wraps
  // this call in ImGui::PushID(stable_id(node)), which TextEditor::Render's
  // internal child window respects, so no per-node suffix is needed here.
  auto label = std::string("##te");
  ImVec2 size{w > 0 ? float(w) : -1.f, h > 0 ? float(h) : -1.f};
  st.editor.Render(label.c_str(), size);

  // TextEditor draws its own blinking caret whenever its window has focus,
  // driven by ImGui::GetTime() -- it doesn't set ImGuiIO::WantTextInput, so
  // the render loop's continuous-redraw check would never fire for it.
  // Mark the session dirty directly so it keeps rendering while focused.
  if (ImGui::IsWindowFocused())
    s.dirty.store(kDirtySettleFrames, std::memory_order_release);

  // Write to disk and emit "changed" when the undo stack advances.
  size_t current_undo = st.editor.GetUndoIndex();
  if (!read_only && current_undo != st.last_undo_index) {
    st.last_undo_index = current_undo;
    if (std::ofstream f(full_path, std::ios::binary); f)
      f << st.editor.GetText();

    dynamic payload;
    payload["file_path"_key] = file_path;
    enqueue_event(s, id, "changed"_key, std::move(payload));
  }

  // "cursor_moved": only tracked when wish_ui_schema is set, so unrelated
  // TextEditor uses (e.g. nano) don't accumulate an otherwise-unconsumed
  // event stream. Mirrors the undo-index diff above.
  if (wish_ui_schema) {
    auto current_cursor = st.editor.GetMainCursorPosition();
    if (current_cursor.line != st.last_cursor.line || current_cursor.index != st.last_cursor.index) {
      st.last_cursor = current_cursor;
      dynamic payload;
      payload["line"_key] = static_cast<int32_t>(current_cursor.line);
      payload["column"_key] = static_cast<int32_t>(current_cursor.index);
      enqueue_event(s, id, "cursor_moved"_key, std::move(payload));
    }
  }

  // Ctrl+S → "saved": signals the client to download the finished file.
  if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    dynamic payload;
    payload["file_path"_key] = file_path;
    enqueue_event(s, id, "saved"_key, std::move(payload));
  }
}

} // namespace bdg::wish
