// MIT License © 2026 Binary Dice Games
/// @file nymph_bind.cpp
/// @brief Implementation of nymph's format validation and data binding.
#include "nymph_bind.hpp"

#include <ui/ui_schema_help.hpp>
#if defined(WISH_IMGUI_ENABLED)
#include <imgui/imgui_renderer.hpp>
#endif

#include <yaml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace bdg::wish::nymph {

using namespace bdg::bison;

namespace {

constexpr int kMaxDepth = 32; // also stops a self-referencing YAML anchor

// Layout classes allowed next to the plot classes, to put several plots in
// one image. Nothing that takes a file path or needs interaction.
constexpr const char* kLayoutTypes[] = {"VerticalLayout", "HorizontalLayout", "Spring", "Label", "Separator"};

/// Where an element sits, which decides what it may be.
enum class place { layout, plot, plot3d };

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool is_plot_class(std::string_view type) {
  return starts_with(type, "Plot") && type != "PlotItem" && type != "Plot3DItem";
}

bool is_layout_class(std::string_view type) {
  return std::any_of(std::begin(kLayoutTypes), std::end(kLayoutTypes), [&](const char* t) { return type == t; });
}

bool is_container(std::string_view type) {
  return type == "Plot" || type == "Plot3D" || type == "VerticalLayout" || type == "HorizontalLayout";
}

/// A parsed YAML document plus the source line its first line sits on.
class yaml_tree {
 public:
  yaml_tree(const std::string& text, int first_line) : first_line_(first_line) {
    yaml_parser_t parser{};
    if (!yaml_parser_initialize(&parser))
      throw std::runtime_error("nymph: could not initialize the YAML parser");
    yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    if (!yaml_parser_load(&parser, &document_)) {
      error failure(first_line_ + static_cast<int>(parser.problem_mark.line),
          static_cast<int>(parser.problem_mark.column) + 1,
          std::string("YAML: ") + (parser.problem ? parser.problem : "syntax error"));
      yaml_parser_delete(&parser);
      throw failure;
    }
    yaml_parser_delete(&parser);
  }

  ~yaml_tree() { yaml_document_delete(&document_); }

  yaml_tree(const yaml_tree&) = delete;
  yaml_tree& operator=(const yaml_tree&) = delete;

  const yaml_node_t* root() { return yaml_document_get_root_node(&document_); }
  const yaml_node_t* node(int index) { return yaml_document_get_node(&document_, index); }

  /// An error positioned at @p n.
  error at(const yaml_node_t* n, const std::string& message) const {
    return error(first_line_ + static_cast<int>(n->start_mark.line), static_cast<int>(n->start_mark.column) + 1,
        message);
  }

  int first_line() const { return first_line_; }

 private:
  yaml_document_t document_{};
  int first_line_;
};

std::string scalar_text(const yaml_node_t* n) {
  return std::string(reinterpret_cast<const char*>(n->data.scalar.value), n->data.scalar.length);
}

bool is_plain(const yaml_node_t* n) {
  return n->data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
}

/// The kinds a plain YAML scalar can stand for (same rules as
/// ui_descriptor.cpp's yaml_scalar_to_json()).
enum class scalar_kind { null, boolean, integer, real, text };

struct scalar_value {
  scalar_kind kind{scalar_kind::text};
  bool boolean{false};
  int32_t integer{0};
  float real{0.0f};
};

scalar_value classify(const yaml_node_t* n) {
  scalar_value out;
  if (!is_plain(n))
    return out;
  const std::string s = scalar_text(n);
  if (s == "true" || s == "yes" || s == "on") {
    out.kind = scalar_kind::boolean;
    out.boolean = true;
  } else if (s == "false" || s == "no" || s == "off") {
    out.kind = scalar_kind::boolean;
  } else if (s == "null" || s == "~" || s.empty()) {
    out.kind = scalar_kind::null;
  } else {
    const char* first = s.data() + (s[0] == '+' ? 1 : 0);
    const char* last = s.data() + s.size();
    int32_t iv = 0;
    double dv = 0.0;
    if (auto r = std::from_chars(first, last, iv); r.ec == std::errc{} && r.ptr == last) {
      out.kind = scalar_kind::integer;
      out.integer = iv;
      out.real = static_cast<float>(iv);
    } else if (auto r2 = std::from_chars(first, last, dv); r2.ec == std::errc{} && r2.ptr == last) {
      out.kind = scalar_kind::real;
      out.real = static_cast<float>(dv);
    }
  }
  return out;
}

/// The column name if @p n is a `$column` reference.
std::optional<std::string> reference_of(const yaml_node_t* n) {
  if (n->type != YAML_SCALAR_NODE)
    return std::nullopt;
  std::string s = scalar_text(n);
  if (s.size() < 2 || s[0] != '$' || s[1] == '$')
    return std::nullopt;
  return s.substr(1);
}

/// Everything bind() needs while walking the tree.
struct binder {
  yaml_tree& yaml;
  const table& data;

  const yaml_node_t* find(const yaml_node_t* mapping, std::string_view key) {
    for (auto* pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
      const yaml_node_t* k = yaml.node(pair->key);
      if (k->type == YAML_SCALAR_NODE && scalar_text(k) == key)
        return yaml.node(pair->value);
    }
    return nullptr;
  }

  int column(const yaml_node_t* n, const std::string& name) {
    int index = data.find_column(name);
    if (index < 0) {
      std::string known;
      for (const auto& h : data.headers)
        known += (known.empty() ? "" : ", ") + h;
      throw yaml.at(n, "unknown column '" + name + "'" +
                           (data.headers.empty() ? " (the data part is empty)" : " (columns: " + known + ")"));
    }
    return index;
  }

  /// Numbers for a list field from one `$column`, or from `[$a, $b, ...]`
  /// interleaved by row.
  std::vector<float> numbers(const std::vector<std::pair<const yaml_node_t*, std::string>>& refs) {
    std::vector<std::vector<float>> columns;
    for (const auto& [n, name] : refs)
      columns.push_back(data.numbers(column(n, name))); // a bad cell is reported at its CSV row
    if (columns.size() == 1)
      return std::move(columns.front());
    std::vector<float> out;
    out.reserve(columns.size() * data.row_count());
    for (size_t r = 0; r < data.row_count(); ++r)
      for (const auto& c : columns)
        out.push_back(c[r]);
    return out;
  }

  void store_numbers(dynamic& obj, bison::key_t key, const field& proto, std::vector<float> values) {
    if (proto.is<std::vector<float>>()) {
      obj[key] = std::move(values);
      return;
    }
    std::vector<int32_t> ints(values.size());
    for (size_t i = 0; i < values.size(); ++i)
      ints[i] = std::isnan(values[i]) ? 0 : static_cast<int32_t>(std::lround(values[i]));
    obj[key] = std::move(ints);
  }

  void set_list(dynamic& obj, bison::key_t key, const std::string& what, const field& proto, const yaml_node_t* v) {
    const bool numeric_list = proto.is<std::vector<float>>() || proto.is<std::vector<int32_t>>();
    if (!numeric_list)
      throw yaml.at(v, what + " takes a single value, not a list");
    std::vector<std::pair<const yaml_node_t*, std::string>> refs;
    std::vector<float> literals;
    for (auto* item = v->data.sequence.items.start; item < v->data.sequence.items.top; ++item) {
      const yaml_node_t* e = yaml.node(*item);
      if (auto ref = reference_of(e)) {
        refs.emplace_back(e, *ref);
        continue;
      }
      scalar_value sv = e->type == YAML_SCALAR_NODE ? classify(e) : scalar_value{};
      if (e->type != YAML_SCALAR_NODE || (sv.kind != scalar_kind::integer && sv.kind != scalar_kind::real))
        throw yaml.at(e, what + " must be a list of numbers or a list of $column references");
      literals.push_back(sv.real);
    }
    if (!refs.empty() && !literals.empty())
      throw yaml.at(v, what + " mixes numbers and $column references; use one or the other");
    store_numbers(obj, key, proto, refs.empty() ? std::move(literals) : numbers(refs));
  }

  void set_reference(dynamic& obj, bison::key_t key, const std::string& what, const field& proto,
      const yaml_node_t* v, const std::string& name) {
    if (proto.is<std::vector<float>>() || proto.is<std::vector<int32_t>>()) {
      store_numbers(obj, key, proto, numbers({{v, name}}));
    } else if (proto.is<std::string>()) {
      const auto& cells = data.columns[static_cast<size_t>(column(v, name))];
      std::string joined;
      for (size_t i = 0; i < cells.size(); ++i)
        joined += (i ? "\n" : "") + cells[i];
      obj[key] = std::move(joined);
    } else {
      throw yaml.at(v, what + " cannot take a $column reference (only list and text fields can)");
    }
  }

  /// Throws unless every `|`-separated name in @p text is one of the named
  /// values of the field @p what ("Type.field") refers to.
  void check_names(const yaml_node_t* v, const std::string& what, const std::string& text) {
    const size_t dot = what.find('.');
    std::vector<std::string> names;
    if (auto info = find_ui_element_class(what.substr(0, dot)))
      for (const auto& f : info->fields)
        if (f.name == what.substr(dot + 1))
          names = f.enum_values;
    if (names.empty())
      throw yaml.at(v, what + " must be an integer");
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t bar = std::min(text.find('|', pos), text.size());
      std::string name = text.substr(pos, bar - pos);
      name.erase(0, name.find_first_not_of(" \t"));
      name.erase(name.find_last_not_of(" \t") + 1);
      if (std::find(names.begin(), names.end(), name) == names.end()) {
        std::string known;
        for (const auto& n : names)
          known += (known.empty() ? "" : ", ") + n;
        throw yaml.at(v, what + ": unknown value '" + name + "' (one of: " + known + ")");
      }
      pos = bar + 1;
    }
  }

  void set_scalar(dynamic& obj, bison::key_t key, const std::string& what, const field& proto,
      const yaml_node_t* v) {
    if (auto ref = reference_of(v)) {
      set_reference(obj, key, what, proto, v, *ref);
      return;
    }
    std::string text = scalar_text(v);
    scalar_value sv = classify(v);
    if (starts_with(text, "$$")) { // an escaped literal '$'
      text.erase(0, 1);
      sv = scalar_value{};
    }
    if (proto.is<std::string>()) {
      obj[key] = text; // `title: 2025` means the text "2025"
    } else if (proto.is<bool>()) {
      if (sv.kind != scalar_kind::boolean)
        throw yaml.at(v, what + " must be true or false");
      obj[key] = sv.boolean;
    } else if (proto.is<float>()) {
      if (sv.kind != scalar_kind::integer && sv.kind != scalar_kind::real)
        throw yaml.at(v, what + " must be a number");
      obj[key] = sv.real;
    } else if (proto.is<int32_t>()) {
      if (sv.kind == scalar_kind::integer) {
        obj[key] = sv.integer;
      } else if (sv.kind == scalar_kind::text) {
        // Named Enum/EnumFlags values (`NoLegend|NoTitle`). The names are
        // checked here because the field's own conversion skips unknown ones.
        check_names(v, what, text);
        field probe = proto;
        try {
          probe = text;
        } catch (const std::runtime_error&) {
          throw yaml.at(v, what + ": '" + text + "' is not a valid value");
        }
        if (!probe.is<int32_t>())
          throw yaml.at(v, what + " must be an integer");
        obj[key] = probe.as<int32_t>();
      } else {
        throw yaml.at(v, what + " must be an integer");
      }
    } else if (proto.is<std::vector<float>>() || proto.is<std::vector<int32_t>>()) {
      throw yaml.at(v, what + " must be a list of numbers or a $column reference");
    } else {
      throw yaml.at(v, what + " cannot be set from a nymph source");
    }
  }

  dynamic_ptr children(const yaml_node_t* v, place where, int depth) {
    auto out = dynamic_ptr{bison::key_t{0U}, {}};
    int32_t order = 0;
    auto add = [&](const yaml_node_t* child_node, const std::string* name, size_t index) {
      dynamic child = element(child_node, where, depth + 1);
      if (!child.findField("order"_key))
        child["order"_key] = order;
      ++order;
      if (name) {
        child["__name__"_key] = *name;
        (*out)[bison::key_t{*name}] = dynamic_ptr{std::move(child)};
      } else {
        (*out)[index] = dynamic_ptr{std::move(child)};
      }
    };
    if (v->type == YAML_SEQUENCE_NODE) {
      size_t index = 0;
      for (auto* item = v->data.sequence.items.start; item < v->data.sequence.items.top; ++item)
        add(yaml.node(*item), nullptr, index++);
    } else if (v->type == YAML_MAPPING_NODE) {
      for (auto* pair = v->data.mapping.pairs.start; pair < v->data.mapping.pairs.top; ++pair) {
        const yaml_node_t* k = yaml.node(pair->key);
        if (k->type != YAML_SCALAR_NODE || scalar_text(k).empty())
          throw yaml.at(k, "a child's name must be plain text");
        std::string name = scalar_text(k);
        add(yaml.node(pair->value), &name, 0);
      }
    } else {
      throw yaml.at(v, "'children' must be a list of elements or a mapping of name to element");
    }
    return out;
  }

  void check_type(const yaml_node_t* at_node, const std::string& type, place where) {
    const bool plot3d_item = starts_with(type, "Plot3D") && type != "Plot3D";
    const bool plot_item = !plot3d_item && starts_with(type, "Plot") && type != "Plot" && type != "Plot3D";
    const bool known = (is_plot_class(type) || is_layout_class(type)) && find_ui_element_class(type).has_value();
    if (!known)
      throw yaml.at(at_node,
          "type '" + type + "' is not allowed here; a nymph image is built from Plot, Plot3D, their series "
                            "(PlotLine, PlotBars, Plot3DSurface, ...) and VerticalLayout, HorizontalLayout, "
                            "Spring, Label, Separator");
    if (where == place::plot && !plot_item)
      throw yaml.at(at_node, "a Plot can only contain 2D series (PlotLine, PlotBars, ...), not '" + type + "'");
    if (where == place::plot3d && !plot3d_item)
      throw yaml.at(at_node, "a Plot3D can only contain 3D series (Plot3DLine, Plot3DSurface, ...), not '" + type + "'");
    if (where == place::layout && plot_item)
      throw yaml.at(at_node, "'" + type + "' must be a child of a Plot");
    if (where == place::layout && plot3d_item)
      throw yaml.at(at_node, "'" + type + "' must be a child of a Plot3D");
  }

  dynamic element(const yaml_node_t* n, place where, int depth) {
    if (depth > kMaxDepth)
      throw yaml.at(n, "elements are nested too deeply");
    if (n->type != YAML_MAPPING_NODE)
      throw yaml.at(n, "an element must be a mapping with a 'type'");
    const yaml_node_t* type_node = find(n, "type");
    if (!type_node || type_node->type != YAML_SCALAR_NODE)
      throw yaml.at(n, "element has no 'type'");
    const std::string type = scalar_text(type_node);
    check_type(type_node, type, where);

    // A fresh instance of the class tells us each field's registered type.
    std::shared_ptr<dynamic> proto(dynamic::create_instance("wish"_key, bison::key_t{type}));
    dynamic obj;
    obj["__type__"_key] = bison::key_t{type};

    for (auto* pair = n->data.mapping.pairs.start; pair < n->data.mapping.pairs.top; ++pair) {
      const yaml_node_t* k = yaml.node(pair->key);
      const yaml_node_t* v = yaml.node(pair->value);
      if (k->type != YAML_SCALAR_NODE)
        throw yaml.at(k, "a field name must be plain text");
      const std::string name = scalar_text(k);
      if (name == "type")
        continue;
      if (name == "children") {
        if (!is_container(type))
          throw yaml.at(k, type + " cannot have children");
        place inner = type == "Plot" ? place::plot : type == "Plot3D" ? place::plot3d : place::layout;
        obj["children"_key] = children(v, inner, depth);
        continue;
      }
      const field* proto_field = starts_with(name, "__") ? nullptr : proto->findField(bison::key_t{name});
      if (!proto_field)
        throw yaml.at(k, type + " has no field '" + name + "'");
      const std::string what = type + "." + name;
      if (v->type == YAML_MAPPING_NODE)
        throw yaml.at(v, what + " cannot be a mapping");
      if (v->type == YAML_SEQUENCE_NODE)
        set_list(obj, bison::key_t{name}, what, *proto_field, v);
      else
        set_scalar(obj, bison::key_t{name}, what, *proto_field, v);
    }
    return obj;
  }

  int integer(const yaml_node_t* v, const std::string& what, int low, int high) {
    scalar_value sv = v->type == YAML_SCALAR_NODE ? classify(v) : scalar_value{};
    if (v->type != YAML_SCALAR_NODE || sv.kind != scalar_kind::integer || sv.integer < low || sv.integer > high)
      throw yaml.at(v, what + " must be an integer from " + std::to_string(low) + " to " + std::to_string(high));
    return sv.integer;
  }

  void image(const yaml_node_t* n, image_options& o) {
    if (n->type != YAML_MAPPING_NODE)
      throw yaml.at(n, "'image' must be a mapping");
    for (auto* pair = n->data.mapping.pairs.start; pair < n->data.mapping.pairs.top; ++pair) {
      const yaml_node_t* k = yaml.node(pair->key);
      const yaml_node_t* v = yaml.node(pair->value);
      const std::string name = k->type == YAML_SCALAR_NODE ? scalar_text(k) : std::string{};
      if (name == "width") {
        o.width = integer(v, "image.width", 16, 4096);
      } else if (name == "height") {
        o.height = integer(v, "image.height", 16, 4096);
      } else if (name == "scale") {
        o.scale = integer(v, "image.scale", 1, 4);
      } else if (name == "padding") {
        o.padding = integer(v, "image.padding", 0, 512);
      } else if (name == "theme") {
        if (v->type != YAML_SCALAR_NODE)
          throw yaml.at(v, "image.theme must be a theme name");
        o.theme = scalar_text(v);
#if defined(WISH_IMGUI_ENABLED)
        if (!find_theme(o.theme))
          throw yaml.at(v, "unknown theme '" + o.theme + "' (light, dark, classic, wish)");
#endif
      } else {
        throw yaml.at(k, "unknown image option '" + name + "' (width, height, scale, theme, padding)");
      }
    }
    if (o.padding * 2 >= o.width || o.padding * 2 >= o.height)
      throw yaml.at(n, "image.padding does not fit the image size");
  }
};

} // namespace

figure bind(const document& doc) {
  figure out;
  out.data = parse_csv(doc.data, doc.data_line);

  yaml_tree yaml(doc.format, doc.format_line);
  const yaml_node_t* top = yaml.root();
  if (!top)
    throw error(doc.format_line, 1, "the format part is empty; it needs a 'root' element");
  binder b{yaml, out.data};
  if (top->type != YAML_MAPPING_NODE)
    throw yaml.at(top, "the format part must be a mapping with a 'root' element");

  const yaml_node_t* root_node = nullptr;
  for (auto* pair = top->data.mapping.pairs.start; pair < top->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* k = yaml.node(pair->key);
    const std::string name = k->type == YAML_SCALAR_NODE ? scalar_text(k) : std::string{};
    if (name == "image")
      b.image(yaml.node(pair->value), out.options);
    else if (name == "root")
      root_node = yaml.node(pair->value);
    else
      throw yaml.at(k, "unknown key '" + name + "'; the format part has 'image' and 'root'");
  }
  if (!root_node)
    throw yaml.at(top, "the format part has no 'root' element");

  out.root = b.element(root_node, place::layout, 0);

  // A lone plot fills the image (and, in edit mode, the preview window)
  // unless it says otherwise: a negative height means "all that is left".
  const yaml_node_t* type_node = b.find(root_node, "type");
  const std::string root_type = scalar_text(type_node);
  if ((root_type == "Plot" || root_type == "Plot3D") && !out.root.findField("height"_key))
    out.root["height"_key] = -1.0f;
  return out;
}

std::vector<std::string> allowed_types() {
  std::vector<std::string> out;
  for (const auto& info : enumerate_ui_element_classes())
    if (is_plot_class(info.name) || is_layout_class(info.name))
      out.push_back(info.name);
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace bdg::wish::nymph
