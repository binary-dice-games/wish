// MIT License © 2025 Binary Dice Games
/// @file imgui_text_editor_renderer.cpp
/// @brief ImGui render function for the TextEditor wish UI element.
#include "imgui_ui_renderer.hpp"

#include "src/bison/bison_object.hpp"

#include <context/file_service.hpp>
#include <context/style_service.hpp>

#include <TextEditor.h>
#include <imgui.h>

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

} // namespace

// ── Render function ───────────────────────────────────────────────────────────

void render_text_editor(imgui_renderer&, const ui_element& node, const context& s) {
  auto file_path = node.get_as<std::string>("file_path"_key, "");
  auto language = node.get_as<std::string>("language"_key, "none");
  auto read_only = node.get_as<bool>("read_only"_key, false);
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

  // Ctrl+S → "saved": signals the client to download the finished file.
  if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    dynamic payload;
    payload["file_path"_key] = file_path;
    enqueue_event(s, id, "saved"_key, std::move(payload));
  }
}

} // namespace bdg::wish
