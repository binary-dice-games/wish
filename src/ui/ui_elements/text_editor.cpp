// MIT License © 2025 Binary Dice Games
/// @file text_editor.cpp
/// @brief Registers the TextEditor prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_text_editor() {
  auto proto = dynamic_ptr{"TextEditor"_rkey, {}};
  proto->addField(
      "file_path"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("File Path"),
          attr<Description>("Path to the file to edit. Relative paths are sandboxed inside the "
                            "session resource directory. Absolute paths require the server to be "
                            "configured with set_allow_absolute_paths(true)."),
          attr<Category>("Content"),
          attr<Required>()});
  proto->addField(
      "language"_rkey,
      field{
          std::string{"none"},
          attr<DisplayName>("Language"),
          attr<Description>("Syntax highlighting language. Supported values: "
                            "\"cpp\", \"c\", \"cs\", \"glsl\", \"hlsl\", \"lua\", "
                            "\"python\", \"sql\", \"json\", \"markdown\", \"angelscript\", "
                            "\"none\"."),
          attr<Category>("Content")});
  proto->addField(
      "read_only"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Read Only"),
          attr<Description>("When true, the editor is read-only and emits no "
                            "\"changed\" events."),
          attr<Category>("Behavior")});
  proto->addField(
      "width"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Width"),
          attr<Description>("Widget width in pixels. 0 = fill available width."),
          attr<Category>("Layout"),
          attr<Range>(0, 8192),
          attr<Step>(1)});
  proto->addField(
      "height"_rkey,
      field{
          int32_t{400},
          attr<DisplayName>("Height"),
          attr<Description>("Widget height in pixels. 0 = fill available height."),
          attr<Category>("Layout"),
          attr<Range>(0, 8192),
          attr<Step>(1)});
  proto->addField(
      "wish_ui_schema"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Wish UI Schema Autocomplete"),
          attr<Description>("When true and language is \"json\", enables cursor-position tracking "
                            "(\"cursor_moved\" events) and autocomplete for wish UI element type "
                            "names, field names, and enum values, sourced from the live class "
                            "registry (see src/ui/ui_schema_help.hpp). Off by default so unrelated "
                            "TextEditor uses (e.g. nano) are unaffected."),
          attr<Category>("Behavior")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Text Editor"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("A full-featured code/text editor backed by ImGuiColorTextEdit. "
                        "Reads and writes a local file; use upload_file / download_file "
                        "for remote transports."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key,
      dynamic::make_factory<ui_text_editor>("wish"_key, "TextEditor"_key));
}

} // namespace bdg::wish
