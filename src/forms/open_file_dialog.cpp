// MIT License © 2025 Binary Dice Games
/// @file open_file_dialog.cpp
/// @brief Implementation of the OpenFileDialog form.
#include "open_file_dialog.hpp"

#include "src/bison/bison_object.hpp"

namespace bdg::wish {

using namespace bison;

// ── open_file_dialog ──────────────────────────────────────────────────────────

open_file_dialog::open_file_dialog(dynamic&& base)
    : form(std::move(base)) {}

void open_file_dialog::on_init() {
  // Internal UI construction added in Step 5.
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_open_file_dialog() {
  auto proto = dynamic_ptr{"OpenFileDialog"_key, {}};

  proto->addField("title"_key, field{std::string{"Open File"},
    attr<DisplayName>("Title"),
    attr<Description>("Dialog window title."),
    attr<Category>("Appearance")});

  proto->addField("files"_key, field{dynamic_ptr{key_t{0U}, {}},
    attr<DisplayName>("Files"),
    attr<Description>(
        "Ordered list of entries. Each entry is a dynamic with "
        "name (string) and type (\"file\" or \"dir\")."),
    attr<Category>("Data")});

  proto->addField("filename"_key, field{std::string{""},
    attr<DisplayName>("Filename"),
    attr<Description>(
        "Current value of the filename input. Client may preset it; "
        "the form updates it as the user types or clicks a row."),
    attr<Category>("Data")});

  proto->addField("filters"_key, field{dynamic_ptr{key_t{0U}, {}},
    attr<DisplayName>("Filters"),
    attr<Description>(
        "Optional list of filter strings shown in a combo box "
        "(e.g. {0: \"*.txt\", 1: \"*.md\"}). "
        "Empty means no filter UI is shown."),
    attr<Category>("Data")});

  proto->addField("confirm_label"_key, field{std::string{"Open"},
    attr<DisplayName>("Confirm Label"),
    attr<Description>("Label on the confirm button."),
    attr<Category>("Appearance")});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("OpenFileDialog"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "High-level file-picker dialog. The client supplies the file list "
      "and reacts to on_open, on_navigate, and on_cancel events."));
  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      dynamic::make_factory<open_file_dialog>(
          "wish"_key, "OpenFileDialog"_key));
}

}  // namespace bdg::wish
