// MIT License © 2026 Binary Dice Games
/// @file conf.cpp
/// @brief conf: the System Settings-like editor of described settings.
#include "conf.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/context.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>
#include <utility>

namespace bdg::wish {

using namespace bdg::bison;
namespace st = settings;
using common::wish_id_of;

namespace {

// ── Look ─────────────────────────────────────────────────────────────────────

constexpr float kSidebarWidth = 250.0f;
constexpr int kSidebarIcon = 24;
constexpr int kHeaderIcon = 64;
constexpr float kEditorWidth = 220.0f;
constexpr const char* kDimLight = "#6E6E73FF";
constexpr const char* kDimDark = "#98989DFF";
constexpr const char* kErrorLight = "#D70015FF";
constexpr const char* kErrorDark = "#FF6961FF";
constexpr const char* kGrayBadge = "#8E8E93";

/// The categories every installation has, in sidebar order.
struct builtin_category {
  const char* id;
  const char* label;
  const char* description;
  const char* icon;
  const char* color;
};
constexpr builtin_category kBuiltins[] = {
    {"general", "$$CONF_CAT_GENERAL", "$$CONF_CAT_GENERAL_DESC", "settings", "#8E8E93"},
    {"appearance", "$$CONF_CAT_APPEARANCE", "$$CONF_CAT_APPEARANCE_DESC", "palette", "#1C1C1E"},
    {"desktop", "$$CONF_CAT_DESKTOP", "$$CONF_CAT_DESKTOP_DESC", "desktop", "#2F7BF5"},
    {"server", "$$CONF_CAT_SERVER", "$$CONF_CAT_SERVER_DESC", "server", "#5E5CE6"},
};

// ── A small JSON layout builder ──────────────────────────────────────────────

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

std::string json_number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return buf;
}

/// One layout node: its type, its fields (raw JSON) and named children.
struct jn {
  std::string type;
  std::vector<std::pair<std::string, std::string>> fields;
  std::vector<std::pair<std::string, jn>> children;

  explicit jn(std::string t) : type(std::move(t)) {}
  jn& s(const std::string& k, const std::string& v) {
    fields.emplace_back(k, json_string(v));
    return *this;
  }
  jn& n(const std::string& k, double v) {
    fields.emplace_back(k, json_number(v));
    return *this;
  }
  jn& b(const std::string& k, bool v) {
    fields.emplace_back(k, v ? "true" : "false");
    return *this;
  }
  jn& raw(const std::string& k, const std::string& v) {
    fields.emplace_back(k, v);
    return *this;
  }
  jn& add(const std::string& name, jn child) {
    children.emplace_back(name, std::move(child));
    return *this;
  }
  std::string dump(int order = -1) const {
    std::string out = "{\"type\":" + json_string(type);
    if (order >= 0)
      out += ",\"order\":" + std::to_string(order);
    for (const auto& [k, v] : fields)
      out += "," + json_string(k) + ":" + v;
    if (!children.empty()) {
      out += ",\"children\":{";
      for (size_t i = 0; i < children.size(); ++i) {
        if (i)
          out += ",";
        out += json_string(children[i].first) + ":" + children[i].second.dump(int(i));
      }
      out += "}";
    }
    return out + "}";
  }
};

jn label(const std::string& text) {
  return std::move(jn("Label").s("text", text));
}

jn dim(const std::string& text, bool wrap = false) {
  jn l = label(text);
  l.s("text_color_light", kDimLight).s("text_color_dark", kDimDark);
  if (wrap)
    l.b("wrap", true);
  return l;
}

jn spring() {
  return jn("Spring");
}

jn badge(const std::string& icon, const std::string& color, int size) {
  return std::move(jn("Image")
                       .s("src", icon)
                       .n("width", size)
                       .n("height", size)
                       .s("badge", color.empty() ? kGrayBadge : color)
                       .s("tint", "#FFFFFFFF"));
}

/// "res/icons/x.png" for a bare name, the path itself otherwise.
std::string icon_src(const std::string& icon) {
  if (icon.empty())
    return "res/icons/settings.png";
  if (icon.find('/') == std::string::npos)
    return "res/icons/" + icon + ".png";
  return icon;
}

/// Path-safe child name for @p text.
std::string slug(const std::string& text) {
  std::string out;
  for (unsigned char c : text)
    out += std::isalnum(c) ? char(c) : '_';
  return out.empty() ? "_" : out;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string capitalize(std::string s) {
  if (!s.empty())
    s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

std::vector<float> color_floats(const std::string& hex) {
  std::vector<float> out{0.0f, 0.0f, 0.0f};
  if (hex.size() < 7 || hex[0] != '#')
    return out;
  auto byte = [&](size_t i) { return float(std::stoi(hex.substr(i, 2), nullptr, 16)) / 255.0f; };
  try {
    out = {byte(1), byte(3), byte(5)};
    if (hex.size() == 9)
      out.push_back(byte(7));
  } catch (...) {
  }
  return out;
}

std::string color_hex(const std::vector<float>& c) {
  std::string out = "#";
  char buf[4];
  for (size_t i = 0; i < c.size() && i < 4; ++i) {
    int v = int(std::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    std::snprintf(buf, sizeof buf, "%02X", v);
    out += buf;
  }
  return out;
}

std::string option_label(const st::setting_option& o) {
  return o.label.empty() ? o.value : o.label;
}

} // namespace

// ── conf ─────────────────────────────────────────────────────────────────────

conf::conf(dynamic&& base) : tool_form(std::move(base)) {
  i18n_prefix_ = "bdg/desktop/conf";
}

void conf::on_init() {
  internal_root_key_ = next_available_key("__conf_");
  admin_ = sess().settings_admin;
  has_user_store_ = sess().user_store != nullptr;

  jn sidebar("VerticalLayout");
  sidebar.n("width", kSidebarWidth).n("height", -1).b("scroll", true).n("spacing", 6);
  sidebar.add("search", jn("InputText").s("hint", "$$CONF_SEARCH").n("width", -1));
  jn account("HorizontalLayout");
  account.n("spacing", 10)
      .add("icon", badge("res/icons/person.png", kGrayBadge, 40))
      .add("text", std::move(jn("VerticalLayout").n("spacing", 2).add("name", label("")).add("role", dim(""))));
  sidebar.add("account", std::move(account));
  sidebar.add("sep", jn("Separator"));
  sidebar.add("list", std::move(jn("VerticalLayout").n("spacing", 2)));

  jn window("Window");
  window.s("title", "$$CONF_TITLE").n("width", 980).n("height", 680);
  window.add(
      "body",
      std::move(
          jn("HorizontalLayout")
              .n("height", -1)
              .n("spacing", 14)
              .add("sidebar", std::move(sidebar))
              .add(
                  "panel",
                  std::move(jn("VerticalLayout").n("width", -1).n("height", -1).b("scroll", true).n("spacing", 14)))));

  const std::string layout = window.dump();
  build_window(internal_root_key_, layout.c_str(), window_id_, [&](ui_tree& tree) {
    auto find = [&](const std::string& path) {
      ui_element_ptr out;
      tree.with(path, [&](const auto& el) { out = el; });
      return out;
    };
    search_id_ = wish_id_of(find("body.sidebar.search"));
    sidebar_list_ = find("body.sidebar.list");
    panel_ = find("body.panel");
    account_name_ = find("body.sidebar.account.text.name");
    account_role_ = find("body.sidebar.account.text.role");
  });

  const std::string& identity = sess().identity;
  (*account_name_)["text"_key] = identity.empty() ? tr("$$CONF_ANONYMOUS") : identity;
  (*account_role_)["text"_key] = tr(admin_ ? "$$CONF_ADMIN" : "$$CONF_USER");

  collect();
  rebuild();
}

// ── Model ────────────────────────────────────────────────────────────────────

persistent_store* conf::store_for(bool server) {
  return server ? sess().server_store.get() : sess().user_store.get();
}

bool conf::collect() {
  std::vector<item> items;
  std::vector<category> categories;
  std::set<std::string> ids;
  for (const auto& b : kBuiltins) {
    category c;
    c.info = {b.id, tr(b.label), tr(b.description), icon_src(b.icon), b.color, 0};
    c.builtin = true;
    ids.insert(b.id);
    categories.push_back(std::move(c));
  }
  std::vector<category> declared;
  auto add_store = [&](persistent_store* store, bool server) {
    if (!store)
      return;
    st::catalog cat = st::collect(*store);
    for (auto& c : cat.categories) {
      if (!ids.insert(c.id).second)
        continue; // built-in, or declared in the other store first
      c.icon = icon_src(c.icon);
      if (c.label.empty())
        c.label = capitalize(c.id);
      declared.push_back({std::move(c), false});
    }
    for (auto& d : cat.settings) {
      if (d.info.category.empty())
        d.info.category = "general";
      items.push_back({std::move(d), server});
    }
  };
  add_store(sess().user_store.get(), false);
  add_store(sess().server_store.get(), true);
  // Settings naming a category nobody declared get a plain one.
  for (const auto& it : items) {
    if (ids.insert(it.setting.info.category).second)
      declared.push_back(
          {{it.setting.info.category, capitalize(it.setting.info.category), "", icon_src(""), "", 0}, false});
  }
  std::stable_sort(declared.begin(), declared.end(), [](const category& a, const category& b) {
    return std::tie(a.info.order, a.info.label) < std::tie(b.info.order, b.info.label);
  });
  for (auto& c : declared)
    categories.push_back(std::move(c));

  std::string sig;
  for (const auto& c : categories)
    sig += st::to_json(c.info) + "\n";
  for (const auto& it : items)
    sig += (it.server ? "S" : "U") + st::to_json(it.setting.info) + "=" + st::to_text(it.setting.value) + "\n";

  items_ = std::move(items);
  categories_ = std::move(categories);
  if (sig == signature_)
    return false;
  signature_ = std::move(sig);
  return true;
}

const conf::item* conf::find_item(const std::string& name, bool server) const {
  for (const auto& it : items_)
    if (it.server == server && it.setting.info.name == name)
      return &it;
  return nullptr;
}

std::vector<const conf::category*> conf::visible_categories() const {
  std::vector<const category*> out;
  for (const auto& c : categories_) {
    bool used = c.info.id == "general" || std::any_of(items_.begin(), items_.end(), [&](const item& it) {
                  return it.setting.info.category == c.info.id;
                });
    if (used)
      out.push_back(&c);
  }
  return out;
}

// ── Building the UI ──────────────────────────────────────────────────────────

void conf::forget(std::vector<key_t>& ids) {
  erase_objects(ids);
  ids.clear();
}

ui_tree conf::attach(const ui_element_ptr& container, const std::string& scope, const std::string& json) {
  ui_tree tree = import_json(json, translations());
  assign_ids(tree);
  // Unique ImGui ids: imported paths are relative to the subtree.
  const std::string prefix = internal_root_key_ + "." + scope;
  for (auto& [path, el] : tree)
    el["__path__"_key] = path.empty() ? prefix : prefix + "." + path;
  auto children = dynamic_ptr{key_t{0U}, {}};
  ui_element_ptr root;
  tree.with("", [&](const auto& el) { root = el; });
  (*children)[size_t{0}] = dynamic_ptr{root};
  (*container)["children"_key] = children;
  container->refresh_children_order();
  return tree;
}

void conf::rebuild() {
  if (std::none_of(categories_.begin(), categories_.end(), [&](const category& c) { return c.info.id == selected_; }))
    selected_ = "general";
  rebuild_sidebar();
  rebuild_panel();
}

void conf::rebuild_sidebar() {
  forget(sidebar_ids_);
  category_rows_.clear();
  auto visible = visible_categories();

  jn list("VerticalLayout");
  list.n("spacing", 2);
  bool apps_group = false;
  std::vector<std::pair<std::string, std::string>> rows; // child name -> category id
  for (size_t i = 0; i < visible.size(); ++i) {
    const category& c = *visible[i];
    if (!c.builtin && !apps_group) {
      apps_group = true;
      list.add("gap", std::move(jn("Label").s("text", "").n("font_size", 6)));
    }
    const std::string name = "c" + std::to_string(i);
    // A rounded Selectable lays out its (leaf) children as a row.
    jn sel("Selectable");
    sel.s("label", "##" + c.info.id)
        .n("width", 0) // ImGui: 0 spans the row (a negative width is not "fill")
        .n("height", kSidebarIcon + 8)
        .n("rounding", 6)
        .b("selected", query_.empty() && c.info.id == selected_)
        .add("icon", badge(c.info.icon, c.info.color, kSidebarIcon))
        .add("text", label(c.info.label));
    list.add(name, std::move(sel));
    rows.emplace_back(name, c.info.id);
  }

  ui_tree tree = attach(sidebar_list_, "sidebar", list.dump());
  for (auto& [path, el] : tree)
    sidebar_ids_.push_back(wish_id_of(el));
  for (const auto& [name, id] : rows) {
    tree.with(name, [&, cat = id](const auto& el) {
      key_t wid = wish_id_of(el);
      category_rows_[wid.id] = cat;
      on_click(wid, [this, cat] { select_category(cat); });
    });
  }
}

void conf::rebuild_panel() {
  forget(panel_ids_);
  editors_.clear();

  struct pending_editor {
    std::string path;
    editor_binding binding;
    std::string error_path;
    std::string reset_path;
  };
  std::vector<pending_editor> pending;
  std::vector<std::pair<std::string, std::pair<std::string, bool>>> resets; // path -> {name, server}

  // One settings row; its editor and companions get bound after import.
  auto make_row = [&](const item& it, const std::string& path) {
    const st::setting_info& info = it.setting.info;
    const st::setting_value& value = it.setting.effective();
    const bool editable = !it.server || admin_;

    jn text("VerticalLayout");
    text.n("width", -1).n("spacing", 2);
    jn title("HorizontalLayout");
    title.n("spacing", 8).add("label", label(info.label.empty() ? st::parse_name(info.name).field : info.label));
    if (it.server)
      title.add("tag", dim("$$CONF_SERVER_TAG"));
    text.add("title", std::move(title));
    if (!info.description.empty())
      text.add("desc", dim(info.description, true));
    if (info.restart)
      text.add("restart", dim("$$CONF_RESTART"));
    if (it.server && !admin_)
      text.add("locked", dim("$$CONF_READ_ONLY"));
    text.add(
        "err",
        std::move(label("")
                      .s("text_color_light", kErrorLight)
                      .s("text_color_dark", kErrorDark)
                      .b("wrap", true)
                      .b("visible", false)));

    jn row("HorizontalLayout");
    row.n("spacing", 12);
    if (!info.icon.empty())
      row.add(
          "icon",
          std::move(jn("Image").s("src", icon_src(info.icon)).n("width", 20).n("height", 20).s("tint", "#8E8E93FF")));
    row.add("text", std::move(text));

    editor_binding binding;
    binding.name = info.name;
    binding.server = it.server;
    if (!editable) {
      std::string shown = st::to_text(value);
      if (info.type == st::setting_type::choice)
        for (const auto& o : info.options)
          if (o.value == shown)
            shown = option_label(o);
      if (info.type == st::setting_type::boolean)
        shown = tr(shown == "true" ? "$$CONF_ON" : "$$CONF_OFF");
      row.add(
          "editor",
          std::move(jn("HorizontalLayout")
                        .n("spacing", 6)
                        .add(
                            "lock",
                            std::move(jn("Image")
                                          .s("src", "res/icons/lock.png")
                                          .n("width", 16)
                                          .n("height", 16)
                                          .s("tint", "#8E8E93FF")))
                        .add("value", dim(shown))));
    } else {
      jn editor("Label");
      switch (info.type) {
        case st::setting_type::boolean: {
          auto* b = std::get_if<bool>(&value);
          editor = std::move(jn("Checkbox").s("label", "").s("style", "switch").b("value", b && *b));
          binding.kind = "Checkbox";
          break;
        }
        case st::setting_type::choice: {
          std::string items;
          int index = -1;
          const std::string current = st::to_text(value);
          for (size_t i = 0; i < info.options.size(); ++i) {
            if (i)
              items += "\n";
            items += option_label(info.options[i]);
            if (info.options[i].value == current)
              index = int(i);
          }
          editor = std::move(jn("Combo").s("label", "").s("items", items).n("value", index).n("width", kEditorWidth));
          binding.kind = "Combo";
          break;
        }
        case st::setting_type::integer: {
          auto* v = std::get_if<int32_t>(&value);
          const int32_t n = v ? *v : 0;
          if (info.min && info.max) {
            editor = std::move(jn("SliderInt")
                                   .s("label", "")
                                   .n("value", n)
                                   .n("min", *info.min)
                                   .n("max", *info.max)
                                   .n("width", kEditorWidth));
            binding.kind = "SliderInt";
          } else {
            editor = std::move(jn("InputInt").s("label", "").n("value", n).n("width", 140));
            binding.kind = "InputInt";
          }
          break;
        }
        case st::setting_type::real: {
          auto* v = std::get_if<float>(&value);
          const float f = v ? *v : 0.0f;
          if (info.min && info.max) {
            editor = std::move(jn("SliderFloat")
                                   .s("label", "")
                                   .n("value", f)
                                   .n("min", *info.min)
                                   .n("max", *info.max)
                                   .n("width", kEditorWidth));
            binding.kind = "SliderFloat";
          } else {
            editor = std::move(jn("InputFloat").s("label", "").n("value", f).n("width", 140));
            binding.kind = "InputFloat";
          }
          break;
        }
        case st::setting_type::color: {
          auto c = color_floats(st::to_text(value));
          std::string floats = "[";
          for (size_t i = 0; i < c.size(); ++i)
            floats += (i ? "," : "") + json_number(c[i]);
          floats += "]";
          editor =
              std::move(jn("ColorEdit").s("label", "").raw("value", floats).n("flags", 32 | (c.size() == 3 ? 2 : 0)));
          binding.kind = "ColorEdit";
          break;
        }
        case st::setting_type::text:
        case st::setting_type::path:
          editor = std::move(jn("InputText").s("label", "").s("value", st::to_text(value)).n("width", kEditorWidth));
          binding.kind = "InputText";
          break;
      }
      row.add("editor", std::move(editor));
      const bool customized = !std::holds_alternative<std::monostate>(it.setting.value) &&
          !std::holds_alternative<std::monostate>(info.default_value) && it.setting.value != info.default_value;
      row.add(
          "reset",
          std::move(jn("Button")
                        .s("label", "")
                        .s("icon", "res/icons/undo.png")
                        .s("tooltip", "$$CONF_RESET")
                        .b("visible", customized)));
      pending.push_back({path + ".editor", binding, path + ".text.err", path + ".reset"});
      resets.push_back({path + ".reset", {info.name, it.server}});
    }
    return row;
  };

  // A card of rows, separated by lines.
  auto make_card = [&](const std::vector<const item*>& rows, const std::string& path) {
    jn card("VerticalLayout");
    card.n("width", -1).b("card", true).n("padding", 14).n("spacing", 10);
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i)
        card.add("s" + std::to_string(i), jn("Separator"));
      card.add("r" + std::to_string(i), make_row(*rows[i], path + ".r" + std::to_string(i)));
    }
    return card;
  };

  jn panel("VerticalLayout");
  panel.n("width", -1).n("spacing", 14);

  if (!query_.empty()) {
    // Search results: every matching setting, grouped by category.
    const std::string q = lower(query_);
    int groups = 0;
    for (const category* c : visible_categories()) {
      std::vector<const item*> matches;
      for (const auto& it : items_) {
        const auto& info = it.setting.info;
        if (info.category == c->info.id &&
            (lower(info.label).find(q) != std::string::npos || lower(info.description).find(q) != std::string::npos ||
             lower(info.name).find(q) != std::string::npos))
          matches.push_back(&it);
      }
      if (matches.empty())
        continue;
      const std::string g = "g" + std::to_string(groups++);
      jn head("HorizontalLayout");
      head.n("spacing", 8).add("icon", badge(c->info.icon, c->info.color, 20)).add("label", label(c->info.label));
      panel.add(g + "h", std::move(head));
      panel.add(g, make_card(matches, g));
    }
    if (groups == 0)
      panel.add("none", dim(trf("$$CONF_NO_RESULTS", {query_})));
  } else {
    const category* cat = nullptr;
    for (const auto& c : categories_)
      if (c.info.id == selected_)
        cat = &c;
    if (cat) {
      // Header card: big badge, title and description, centered.
      jn header("VerticalLayout");
      header.n("width", -1).b("card", true).n("padding", 18).n("spacing", 8);
      header.add(
          "icon",
          std::move(jn("HorizontalLayout")
                        .add("l", spring())
                        .add("i", badge(cat->info.icon, cat->info.color, kHeaderIcon))
                        .add("r", spring())));
      header.add(
          "title",
          std::move(jn("HorizontalLayout")
                        .add("l", spring())
                        .add("t", label(cat->info.label).n("font_size", 26))
                        .add("r", spring())));
      if (!cat->info.description.empty())
        header.add(
            "desc",
            std::move(
                jn("HorizontalLayout").add("l", spring()).add("d", dim(cat->info.description)).add("r", spring())));
      panel.add("header", std::move(header));

      // One card per section, in first-appearance order.
      std::vector<std::string> sections;
      std::map<std::string, std::vector<const item*>> by_section;
      for (const auto& it : items_) {
        if (it.setting.info.category != cat->info.id)
          continue;
        const std::string& sec = it.setting.info.section;
        if (!by_section.count(sec))
          sections.push_back(sec);
        by_section[sec].push_back(&it);
      }
      if (sections.empty() && !has_user_store_)
        panel.add("anon", dim("$$CONF_NO_IDENTITY", true));
      else if (sections.empty())
        panel.add("empty", dim("$$CONF_EMPTY"));
      for (size_t i = 0; i < sections.size(); ++i) {
        const std::string sid = "sec" + std::to_string(i);
        if (!sections[i].empty())
          panel.add(sid + "t", dim(sections[i]).n("font_size", 13));
        panel.add(sid, make_card(by_section[sections[i]], sid));
      }
    }
  }

  ui_tree tree = attach(panel_, "panel", panel.dump());
  for (auto& [path, el] : tree)
    panel_ids_.push_back(wish_id_of(el));
  for (auto& p : pending) {
    ui_element_ptr editor, err, reset;
    tree.with(p.path, [&](const auto& el) { editor = el; });
    tree.with(p.error_path, [&](const auto& el) { err = el; });
    tree.with(p.reset_path, [&](const auto& el) { reset = el; });
    if (!editor)
      continue;
    p.binding.error_label = err;
    p.binding.reset_button = reset;
    editors_[wish_id_of(editor).id] = p.binding;
  }
  for (const auto& [path, target] : resets) {
    tree.with(path, [&, name = target.first, server = target.second](const auto& el) {
      on_click(wish_id_of(el), [this, name, server] { on_reset(name, server); });
    });
  }
}

// ── Interaction ──────────────────────────────────────────────────────────────

void conf::select_category(const std::string& id) {
  selected_ = id;
  query_.clear();
  collect();
  rebuild();
}

void conf::on_edit(const editor_binding& binding, const dynamic& payload) {
  const item* it = find_item(binding.name, binding.server);
  if (!it)
    return;
  if (binding.server && !admin_)
    return; // read-only for this session, whatever the client sends
  persistent_store* store = store_for(binding.server);
  if (!store)
    return;
  const st::setting_info& info = it->setting.info;

  st::setting_value value;
  const field* v = payload.findField("value"_key);
  if (!v)
    return;
  if (binding.kind == "Checkbox" && v->is<bool>()) {
    value = v->as<bool>();
  } else if (binding.kind == "Combo") {
    int32_t index = v->get_as<int32_t>();
    if (index < 0 || size_t(index) >= info.options.size())
      return;
    value = info.options[size_t(index)].value;
  } else if ((binding.kind == "SliderInt" || binding.kind == "InputInt")) {
    value = v->get_as<int32_t>();
  } else if (binding.kind == "SliderFloat" || binding.kind == "InputFloat") {
    value = v->get_as<float>();
  } else if (binding.kind == "ColorEdit" && v->is<std::vector<float>>()) {
    value = color_hex(v->as<std::vector<float>>());
  } else if (binding.kind == "InputText" && v->is<std::string>()) {
    value = v->as<std::string>();
  } else {
    return;
  }

  const std::string error = st::write_setting(*store, info, value);
  if (binding.error_label) {
    (*binding.error_label)["text"_key] = error;
    (*binding.error_label)["visible"_key] = !error.empty();
  }
  if (!error.empty())
    return;
  // The editor already shows the new value: refresh the model without
  // rebuilding, so the editor keeps its focus.
  collect();
  if (const item* now = find_item(binding.name, binding.server); now && binding.reset_button) {
    const auto& d = now->setting;
    (*binding.reset_button)["visible"_key] = !std::holds_alternative<std::monostate>(d.value) &&
        !std::holds_alternative<std::monostate>(d.info.default_value) && d.value != d.info.default_value;
  }
}

void conf::on_reset(const std::string& name, bool server) {
  if (server && !admin_)
    return;
  const item* it = find_item(name, server);
  persistent_store* store = store_for(server);
  if (!it || !store)
    return;
  st::reset_setting(*store, it->setting.info);
  collect();
  rebuild_panel();
}

void conf::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }
  run_in_dispatch([&] {
    if (id == search_id_ && event == "changed"_key) {
      query_ = payload.get_as<std::string>("value"_key, std::string{});
      collect();
      rebuild();
      return;
    }
    if (auto it = editors_.find(id.id); it != editors_.end() && event == "changed"_key) {
      const editor_binding binding = it->second; // copy: on_edit may rebuild
      on_edit(binding, payload);
      return;
    }
    if (event == "clicked"_key || event == "changed"_key)
      dispatch_click(id);
  });
}

// ── RMI ──────────────────────────────────────────────────────────────────────

dynamic conf::do_refresh(const dynamic&) {
  dynamic result;
  const bool changed = collect();
  if (changed)
    rebuild();
  result["changed"_key] = changed;
  return result;
}

dynamic conf::do_select(const dynamic& args) {
  select_category(args.get_as<std::string>("id"_key, std::string{"general"}));
  return dynamic{};
}

void register_conf() {
  auto proto = dynamic_ptr{"Conf"_key, {}};
  proto->addMethod("refresh"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<conf&>(self).do_refresh(args);
                   }});
  proto->addMethod("select"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<conf&>(self).do_select(args);
                   }});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Conf"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Settings editor in the style of macOS System Settings, built from the settings "
                        "metadata apps record in the user and server stores (docs/settings.md). "
                        "Listen for the 'closed' event to detect when the user is done."));
  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<conf>("wish"_key, "Conf"_key));
}

} // namespace bdg::wish
