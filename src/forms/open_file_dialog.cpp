// MIT License © 2025 Binary Dice Games
/// @file open_file_dialog.cpp
/// @brief Implementation of the OpenFileDialog form.
#include "open_file_dialog.hpp"

#include "src/bison/bison_object.hpp"

#include <wish/ui_importer.hpp>

namespace bdg::wish {

using namespace bison;

// ── Hardcoded UI layout ───────────────────────────────────────────────────────

// Placeholder label values are overwritten in on_init() from the form's fields.
static constexpr const char* kDialogLayout = R"({
  "type": "Window",
  "width": 480, "height": 360,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "path_label": { "type": "Label", "text": "" },
        "file_table": {
          "type": "Table",
          "columns": 2,
          "headers": true,
          "children": [
            { "type": "TableColumn", "label": "Name" },
            { "type": "TableColumn", "label": "Type" }
          ]
        },
        "filename_row": {
          "type": "HorizontalLayout",
          "children": {
            "filename_input": {
              "type": "InputText", "value": "", "hint": "filename"
            }
          }
        },
        "filter_row": {
          "type": "HorizontalLayout",
          "visible": false,
          "children": {
            "filter_combo": { "type": "Combo", "label": "" }
          }
        },
        "btn_row": {
          "type": "HorizontalLayout",
          "children": {
            "btn_open":   { "type": "Button", "label": "Open"   },
            "btn_cancel": { "type": "Button", "label": "Cancel" }
          }
        }
      }
    }
  }
})";

// ── open_file_dialog ──────────────────────────────────────────────────────────

open_file_dialog::open_file_dialog(dynamic&& base)
    : form(std::move(base)) {}

void open_file_dialog::on_init() {
  // Derive unique root key from object address; the form's bison ID is not
  // yet available when on_init() is called (assigned after on_create_object).
  internal_root_key_ = "__form_" +
      std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kDialogLayout);

  // Apply form field values to the imported tree.
  auto* title_f = findField("title"_key);
  (*tree[""])["title"_key] =
      title_f ? title_f->as<std::string>() : std::string{"Open File"};

  auto* confirm_f = findField("confirm_label"_key);
  if (auto it = tree.find("vbox.btn_row.btn_open"); it != tree.end()) {
    (*it->second)["label"_key] =
        confirm_f ? confirm_f->as<std::string>() : std::string{"Open"};
  }

  // Merge the imported name_map into session.objects under our prefix.
  auto& objs = sess().objects;
  for (auto& [key, ptr] : tree) {
    objs[key.empty() ? internal_root_key_
                     : (internal_root_key_ + "." + key)] = ptr;
  }
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
