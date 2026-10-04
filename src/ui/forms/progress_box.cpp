// MIT License © 2026 Binary Dice Games
/// @file progress_box.cpp
/// @brief Implementation of the ProgressBox form.
#include "progress_box.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/ui_importer.hpp>

#include <algorithm>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// Optional string field (absent -> "").
std::string str_of(const dynamic& d, key_t key) {
  const auto* f = d.findField<std::string>(key);
  return f ? *f : std::string{};
}

// "#RRGGBBAA" light/dark pairs (GitHub Primer tokens, as the dev modules use).
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

// A fixed size rather than MessageBox's AlwaysAutoResize, so appended output
// never resizes the dialog. No child carries an explicit "width" inside a
// HorizontalLayout -- see message_box.cpp on why that breaks hit-testing in
// a modal. The Table pairs "height": -1 (stretch row) with "outer_height":
// -1 (fill that region).
static constexpr const char* kLayout = R"json({
  "type": "Window", "title": "", "modal": true, "width": 720, "height": 420,
  "flags": "NoResize|NoCollapse",
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 6, "children": {
    "command": { "type": "Label", "text": "" },
    "bar":     { "type": "ProgressBar", "value": -0.001, "label": "", "width": -1 },
    "result":  { "type": "Label", "text": "", "wrap": true, "visible": false },
    "table": {
      "type": "Table", "id": "##progress_box_log", "columns": 1,
      "flags": "RowBg|Borders|ScrollX|ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_line": { "type": "TableColumn", "label": "Output", "flags": "WidthStretch", "column_id": 0 }
      }
    },
    "btn_cancel": { "type": "Button", "label": "Cancel" }
  } } }
})json";

} // namespace

progress_box::progress_box(dynamic&& base) : cloneable_ui_element(std::move(base)) {}

void progress_box::on_init() {
  build();
}

void progress_box::on_construct(const dynamic& params) {
  const auto* title = params.findField<std::string>("title"_key);
  if (!title)
    return;
  (*this)["title"_key] = *title;
  if (window_)
    window_["title"_key] = *title;
}

void progress_box::with_context(const std::function<void()>& fn) {
  // The idiom form::instantiate_child_form() uses: on_event() runs outside
  // RMI dispatch, while building / erasing top-level objects needs sess().
  if (detail::current_context) {
    fn();
    return;
  }
  auto lock = context_wlock{*sync_ctx_};
  detail::current_context = &*lock;
  fn();
  detail::current_context = nullptr;
}

void progress_box::build() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__progress_box_");

  auto tree = import_json(kLayout);
  const auto* title = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title ? *title : std::string{"Working"};

  auto& c = ctx();
  object_ids_.clear();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
    object_ids_.push_back(id);
  }

  window_ = tree[""];
  window_id_ = wish_id_of(window_);
  tree.with("vbox.command", [&](const auto& e) { command_label_ = e; });
  tree.with("vbox.bar", [&](const auto& e) { bar_ = e; });
  tree.with("vbox.result", [&](const auto& e) { result_label_ = e; });
  tree.with("vbox.table", [&](const auto& e) { table_ = e; });
  tree.with("vbox.btn_cancel", [&](const auto& e) {
    button_ = e;
    button_id_ = wish_id_of(e);
  });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);

  state_ = phase::open;
  finished_ = false;
  cancelling_ = false;
  reopen_ = false;
  command_.clear();
  rows_.clear();
  next_child_key_ = 0;
}

void progress_box::register_root() {
  auto& s = sess();
  auto it = s.ui_objects.find(internal_root_key_);
  if (it == s.ui_objects.end())
    return;
  s.top_level_objects[key_t{internal_root_key_}] = it->second;
  (*it->second)["__path__"_key] = internal_root_key_;
  s.top_level_handlers[key_t{internal_root_key_}] = this;
}

void progress_box::request_close() {
  if (state_ != phase::open)
    return;
  state_ = phase::closing;
  if (window_)
    window_["__request_close__"_key] = true;
}

void progress_box::destroy() {
  for (auto id : object_ids_)
    ctx().objects.erase(id.id);
  for (auto& row : rows_) {
    for (auto id : row.object_ids)
      ctx().objects.erase(id.id);
  }
  object_ids_.clear();
  rows_.clear();
  window_ = command_label_ = bar_ = result_label_ = table_ = button_ = ui_element_ptr{};
  remove_internal_objects();
  state_ = phase::closed;
}

void progress_box::append_line(const std::string& text, bool is_command) {
  if (!table_)
    return;
  auto* children_p = table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  auto with_id = [this](const ui_element_ptr& el) {
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, el);
    el["__wish_id"_key] = id;
    return id;
  };

  ui_element_ptr cell = ui_element_ptr::create("wish"_key, "Label"_key);
  cell["text"_key] = is_command ? "$ " + text : text;
  if (is_command) {
    cell["text_color_light"_key] = std::string{kOkLight};
    cell["text_color_dark"_key] = std::string{kOkDark};
  }
  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);

  log_row entry;
  entry.object_ids = {with_id(row), with_id(cell)};
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  (*row_children)[size_t{0}] = dynamic_ptr{cell};
  (*row)["children"_key] = row_children;
  row->refresh_children_order();

  entry.child_key = next_child_key_++;
  (*children)[entry.child_key] = dynamic_ptr{row};
  rows_.push_back(std::move(entry));

  if (rows_.size() > kMaxRows) {
    for (auto id : rows_.front().object_ids)
      ctx().objects.erase(id.id);
    children->erase(rows_.front().child_key);
    rows_.pop_front();
  }
}

dynamic progress_box::do_update(const dynamic& args) {
  if (state_ == phase::closing) {
    reopen_ = true; // rebuilt once the renderer confirms the close.
    return dynamic{};
  }
  if (state_ == phase::closed) {
    build();
    register_root();
  }

  if (finished_) { // a new operation started behind a displayed failure.
    finished_ = false;
    if (bar_)
      bar_["visible"_key] = true;
    if (result_label_)
      result_label_["visible"_key] = false;
    if (button_)
      button_["label"_key] = std::string{"Cancel"};
  }

  const std::string command = str_of(args, "command"_key);
  if (command != command_) {
    command_ = command;
    if (!command.empty())
      append_line(command, /*is_command=*/true);
  }
  if (command_label_ && !cancelling_)
    command_label_["text"_key] = command;

  const auto* lines = args.findField<dynamic_ptr>("lines"_key);
  if (lines && *lines) {
    (*lines)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        append_line(f.as<std::string>(), /*is_command=*/false);
    });
  }
  if (table_)
    table_->refresh_children_order();

  // A known fraction draws a determinate bar with the caller's detail
  // overlaid (empty: ImGui's percentage). Otherwise a negative ProgressBar
  // value draws ImGui's indeterminate animation, whose position follows the
  // value -- hence the ever-growing phase.
  if (bar_) {
    const auto* fraction = args.findField<float>("fraction"_key);
    if (fraction && *fraction >= 0.0f) {
      bar_["value"_key] = std::min(*fraction, 1.0f);
      bar_["label"_key] = str_of(args, "detail"_key);
    } else {
      const auto* seconds = args.findField<float>("phase"_key);
      bar_["value"_key] = -0.001f - (seconds ? *seconds : 0.0f) * 0.5f;
      bar_["label"_key] = std::string{};
    }
  }
  return dynamic{};
}

dynamic progress_box::do_finish(const dynamic& args) {
  reopen_ = false;
  if (state_ != phase::open)
    return dynamic{};

  const std::string error = str_of(args, "error"_key);
  if (error.empty() || cancelling_) {
    request_close();
    return dynamic{};
  }

  // Keep a failure on screen, with the output that led to it.
  finished_ = true;
  if (command_label_)
    command_label_["text"_key] = std::string{"Finished with an error"};
  if (bar_)
    bar_["visible"_key] = false;
  if (result_label_) {
    result_label_["text"_key] = error;
    result_label_["text_color_light"_key] = std::string{kBadLight};
    result_label_["text_color_dark"_key] = std::string{kBadDark};
    result_label_["visible"_key] = true;
  }
  if (button_)
    button_["label"_key] = std::string{"Close"};
  return dynamic{};
}

void progress_box::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  // The renderer confirmed the modal closed (see request_close()).
  if (event == "closed"_key && id == window_id_ && state_ != phase::closed) {
    const bool reopen = reopen_;
    with_context([&] {
      destroy();
      if (reopen) {
        build();
        register_root();
      }
    });
    if (!reopen)
      emit("closed"_key);
    return;
  }

  if (event != "clicked"_key || id != button_id_ || state_ != phase::open)
    return;
  if (finished_) {
    request_close();
    return;
  }
  if (cancelling_)
    return;
  cancelling_ = true;
  if (command_label_)
    command_label_["text"_key] = std::string{"Cancelling ..."};
  emit("cancel_requested"_key);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_progress_box() {
  auto proto = dynamic_ptr{"ProgressBox"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Working"},
          attr<DisplayName>("Title"),
          attr<Description>("Dialog window title."),
          attr<Category>("Appearance")});

  proto->addMethod("__construct"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     static_cast<progress_box&>(s).on_construct(p);
                     return dynamic{};
                   }});
  proto->addMethod("update"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<progress_box&>(s).do_update(p);
                   }});
  proto->addMethod("finish"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<progress_box&>(s).do_finish(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("ProgressBox"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Modal progress dialog for a long-running client-side operation: a progress bar (indeterminate, or "
      "determinate when update() passes a fraction), a scrolling output log and a Cancel button. Call "
      "update({command, phase, lines, fraction, detail}) while the operation "
      "runs and finish({error}) when it ends (a non-empty error keeps the dialog open until the user closes "
      "it). Listen for the cancel_requested and closed events."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<progress_box>("wish"_key, "ProgressBox"_key));
}

} // namespace bdg::wish
