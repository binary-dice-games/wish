// MIT License © 2025 Binary Dice Games
/// @file file_browser_utils.cpp
/// @brief Implementation of shared file-listing helpers -- see the header
/// for the FileDialog/FileExplorer duplication this factors out.
#include "file_browser_utils.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace bdg::wish {

using namespace bison;

std::string icon_for_entry(const std::string& name, const std::string& type) {
  if (type == "dir")
    return "folder";

  auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 == name.size())
    return "file";
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::vector<std::string> kImageExts{"png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "svg"};
  static const std::vector<std::string> kAudioExts{"mp3", "wav", "ogg", "flac", "m4a", "aac"};
  static const std::vector<std::string> kCodeExts{
      "cpp", "hpp", "c", "h", "cc", "cs", "py", "js", "ts", "java", "go", "rs",
      "sh", "json", "yaml", "yml", "xml", "html", "css"};
  static const std::vector<std::string> kDocumentExts{"txt", "md", "pdf", "doc", "docx", "rtf", "log"};

  auto contains = [&](const std::vector<std::string>& exts) {
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
  };

  if (contains(kImageExts))
    return "image";
  if (contains(kAudioExts))
    return "audio";
  if (contains(kCodeExts))
    return "code";
  if (contains(kDocumentExts))
    return "document";
  return "file";
}

ui_element_ptr make_name_cell(const std::string& name, const std::string& type, const std::string& display_name) {
  ui_element_ptr icon_row{dynamic::instantiate("wish"_key, "HorizontalLayout"_key)};
  icon_row["spacing"_key] = 6.0f;
  icon_row["order"_key] = int32_t{0};

  ui_element_ptr icon_img{dynamic::instantiate("wish"_key, "Image"_key)};
  icon_img["src"_key] = "res/icons/" + icon_for_entry(name, type) + ".png";
  icon_img["__auto_size_to_font__"_key] = true;
  // The icon PNGs are white/monochrome so they can be tinted; without this
  // they're invisible against the light theme's white background (see
  // render_image()'s "__tint_to_text_color__" handling).
  icon_img["__tint_to_text_color__"_key] = true;
  icon_img["order"_key] = int32_t{0};

  ui_element_ptr name_lbl{dynamic::instantiate("wish"_key, "Label"_key)};
  name_lbl["text"_key] = display_name;
  name_lbl["order"_key] = int32_t{1};

  auto icon_row_children = dynamic_ptr{key_t{0U}, {}};
  (*icon_row_children)[size_t{0}] = dynamic_ptr{icon_img};
  (*icon_row_children)[size_t{1}] = dynamic_ptr{name_lbl};
  icon_row["children"_key] = icon_row_children;

  return icon_row;
}

} // namespace bdg::wish
