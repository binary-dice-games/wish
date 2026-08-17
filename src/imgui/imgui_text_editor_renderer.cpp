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
  std::string applied_preset; // palette preset last applied ("dark", "light", ...)
  size_t last_undo_index{0};
  TextEditor::DocPos last_cursor{}; // only tracked when wish_ui_schema is true
  bool autocomplete_configured{false}; // whether SetAutoCompleteConfig has been applied
};

std::unordered_map<uint32_t, TextEditorState>& editor_cache() {
  static std::unordered_map<uint32_t, TextEditorState> cache;
  return cache;
}

// Map wish language string → TextEditor::Language factory.
const TextEditor::Language* language_for(const std::string& lang) {
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
  auto ctx = scan_cursor_context(editor->GetText(), pos);

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

void render_text_editor(imgui_renderer&, const ui_element& node, const context& s) {
  auto file_path = node.get_as<std::string>("file_path"_key, "");
  auto language = node.get_as<std::string>("language"_key, "none");
  auto read_only = node.get_as<bool>("read_only"_key, false);
  auto wish_ui_schema = node.get_as<bool>("wish_ui_schema"_key, false);
  int32_t w = node.get_as<int32_t>("width"_key, 0);
  int32_t h = node.get_as<int32_t>("height"_key, 400);

  if (file_path.empty())
    return;

  auto full_path = file_service::resolve_path(file_path, s.resource_dir, s.allow_absolute_paths);
  if (full_path.empty())
    return;

  auto id = node.get_as<key_t>("__wish_id"_key, key_t{});
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
  // doc comment) so unrelated TextEditor uses (e.g. Notepad) are unaffected.
  // AutoCompleteConfig::setConfig() copies *cfg by value, so a stack local
  // is safe here -- it does not need to outlive this call.
  if (wish_ui_schema && language == "json") {
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

  // Sync the TextEditor palette with the session's active style preset.
  // TextEditor maintains its own color palette independent of ImGuiStyle.
  {
    std::string preset = "dark";
    if (s.style_service) {
      const auto* f = s.style_service->current_style().findField("preset"_key);
      if (f && f->is<std::string>())
        preset = f->as<std::string>();
    }
    if (st.applied_preset != preset) {
      st.applied_preset = preset;
      st.editor.SetPalette(preset == "light" ? TextEditor::GetLightPalette() : TextEditor::GetDarkPalette());
    }
  }

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
  // TextEditor uses (e.g. Notepad) don't accumulate an otherwise-unconsumed
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
