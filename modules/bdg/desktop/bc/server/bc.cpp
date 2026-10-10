// MIT License © 2025 Binary Dice Games
/// @file bc.cpp
/// @brief Implementation of the bc form: builds the multi-mode calculator UI
///        and routes key presses to calc::engine.
#include "bc.hpp"

#include "src/bison/bison_object.hpp"

#include <ui/ui_importer.hpp>

#include <algorithm>
#include <sstream>

namespace bdg::wish {

using namespace bison;
using common::wish_id_of;

namespace {

// UTF-8 key glyphs (the default font covers these), written as escapes so
// the source stays ASCII.
constexpr const char* kTimes = "\xC3\x97";
constexpr const char* kDivide = "\xC3\xB7";
constexpr const char* kMinus = "\xE2\x88\x92";
constexpr const char* kSqrt = "\xE2\x88\x9A";
constexpr const char* kPiGlyph = "\xCF\x80";
constexpr const char* kSquared = "\xC2\xB2";
constexpr const char* kCubed = "\xC2\xB3";
constexpr const char* kPlusMinus = "\xC2\xB1";
constexpr const char* kBackspace = "\xE2\x86\x90";
constexpr const char* kOn = "\xE2\x80\xA2 "; // "on" marker of toggle keys

constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

constexpr const char* kModeNames[] = {"Standard", "Scientific", "Programmer"};
constexpr const char* kKeypadIds[] = {"std", "sci", "prog"};
constexpr int kRadixes[] = {16, 10, 8, 2};
constexpr const char* kRadixNames[] = {"HEX", "DEC", "OCT", "BIN"};
constexpr const char* kRadixIds[] = {"hex", "dec", "oct", "bin"};

// Layout metrics shared by make_layout() and fit_window().
constexpr int kKeySpacing = 4;
constexpr int kTabWidth = 104;
constexpr int kRadixWidth = 7 * 72 + 6 * kKeySpacing; // the programmer keypad's width
constexpr int kRadixRowHeight = 22;
constexpr int kRadixSpacing = 2;
constexpr int kRadixHeight = 4 * kRadixRowHeight + 3 * kRadixSpacing;
constexpr int kHistoryGap = 12;
constexpr int kHistoryWidth = 240;
// Title + 10 entries of 40 px + Clear button, with spacing.
constexpr int kHistoryHeight = 24 + 18 + 10 * 44 + 30;

// Dimmed secondary text (expression line, status line), per theme.
constexpr const char* kDimLight = "#656D76FF";
constexpr const char* kDimDark = "#8B949EFF";

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string str(const std::string& a, const std::string& b) {
  return a + b;
}

const char* angle_name(calc::angle_unit u) {
  switch (u) {
    case calc::angle_unit::degrees:
      return "DEG";
    case calc::angle_unit::radians:
      return "RAD";
    case calc::angle_unit::gradians:
      return "GRAD";
  }
  return "";
}

const char* word_name(calc::word_size w) {
  switch (w) {
    case calc::word_size::qword:
      return "QWORD";
    case calc::word_size::dword:
      return "DWORD";
    case calc::word_size::word:
      return "WORD";
    case calc::word_size::byte:
      return "BYTE";
  }
  return "";
}

} // namespace

// ── bc ────────────────────────────────────────────────────────────────────────

bc::bc(dynamic&& base) : tool_form(std::move(base)) {}

std::vector<bc::keypad_def> bc::make_keypads() {
  using calc::binary_op;
  using calc::unary_op;
  auto& e = engine_;

  auto digit = [&e](int d) {
    std::string label(1, static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
    return key_def{std::string{"n"} + label, label, [&e, d] { e.digit(d); }};
  };
  auto bin = [&e](const char* id, std::string label, binary_op op) {
    return key_def{id, std::move(label), [&e, op] { e.binary(op); }};
  };
  auto un = [&e](const char* id, std::string label, unary_op op) {
    return key_def{id, std::move(label), [&e, op] { e.unary(op); }};
  };
  // A key whose function and label switch while "2nd" is on.
  auto second =
      [this](const char* id, std::string label, std::string label2, std::function<void()> a, std::function<void()> a2) {
        return key_def{
            id,
            label,
            [this, a = std::move(a), a2 = std::move(a2)] {
              (second_ ? a2 : a)();
              second_ = false;
            },
            [this, label, label2] { return second_ ? label2 : label; }};
      };
  auto un2 = [&](const char* id, std::string label, unary_op op, std::string label2, unary_op op2) {
    return second(id, std::move(label), std::move(label2), [&e, op] { e.unary(op); }, [&e, op2] { e.unary(op2); });
  };
  // sin / cos / tan with 2nd (inverse) and hyp (hyperbolic) variants.
  auto trig = [this, &e](const char* id, const char* name, unary_op f, unary_op inv, unary_op h, unary_op hinv) {
    std::string n{name};
    return key_def{
        id,
        n,
        [this, &e, f, inv, h, hinv] {
          e.unary(hyp_ ? (second_ ? hinv : h) : (second_ ? inv : f));
          second_ = false;
          hyp_ = false;
        },
        [this, n] { return (second_ ? "a" : "") + n + (hyp_ ? "h" : ""); }};
  };
  auto toggle_label = [](std::string label, std::function<bool()> on) {
    return [label = std::move(label), on = std::move(on)] { return on() ? kOn + label : label; };
  };
  auto word = [&, toggle_label](const char* id, calc::word_size w) {
    return key_def{id, word_name(w), [&e, w] { e.set_word_size(w); }, toggle_label(word_name(w), [&e, w] {
                     return e.get_word_size() == w;
                   })};
  };

  key_def ce{"ce", "CE", [&e] { e.clear_entry(); }};
  key_def clr{"c", "C", [&e] { e.clear(); }};
  key_def bsp{"bsp", kBackspace, [&e] { e.backspace(); }};
  key_def neg{"neg", kPlusMinus, [&e] { e.negate(); }};
  key_def dot{"dot", ".", [&e] { e.point(); }};
  key_def eq{"eq", "=", [&e] { e.equals(); }};
  key_def lp{"lp", "(", [&e] { e.open_paren(); }};
  key_def rp{"rp", ")", [&e] { e.close_paren(); }};
  key_def add = bin("add", "+", binary_op::add);
  key_def sub = bin("sub", kMinus, binary_op::subtract);
  key_def mul = bin("mul", kTimes, binary_op::multiply);
  key_def div = bin("div", kDivide, binary_op::divide);
  key_def mod = bin("mod", "mod", binary_op::modulo);

  keypad_def standard{
      "std",
      76,
      48,
      {
          {{"pct", "%", [&e] { e.percent(); }}, ce, clr, bsp},
          {un("inv", "1/x", unary_op::reciprocal),
           un("sq", str("x", kSquared), unary_op::square),
           un("sqrt", str(kSqrt, "x"), unary_op::sqrt),
           div},
          {digit(7), digit(8), digit(9), mul},
          {digit(4), digit(5), digit(6), sub},
          {digit(1), digit(2), digit(3), add},
          {neg, digit(0), dot, eq},
      }};

  keypad_def scientific{
      "sci",
      58,
      40,
      {
          {{"2nd", "2nd", [this] { second_ = !second_; }, toggle_label("2nd", [this] { return second_; })},
           {"hyp", "hyp", [this] { hyp_ = !hyp_; }, toggle_label("hyp", [this] { return hyp_; })},
           {"angle",
            "DEG",
            [&e] {
              auto u = e.get_angle_unit();
              e.set_angle_unit(
                  u == calc::angle_unit::degrees       ? calc::angle_unit::radians
                      : u == calc::angle_unit::radians ? calc::angle_unit::gradians
                                                       : calc::angle_unit::degrees);
            },
            [&e] { return std::string{angle_name(e.get_angle_unit())}; }},
           {"fe",
            "F-E",
            [&e] { e.set_scientific_notation(!e.scientific_notation()); },
            toggle_label("F-E", [&e] { return e.scientific_notation(); })},
           {"rand", "rand", [&e] { e.random(); }},
           ce,
           clr,
           bsp,
           div},
          {lp,
           rp,
           mod,
           {"exp", "exp", [&e] { e.exponent(); }},
           un("fact", "n!", unary_op::factorial),
           digit(7),
           digit(8),
           digit(9),
           mul},
          {un2("sq", str("x", kSquared), unary_op::square, str("x", kCubed), unary_op::cube),
           second(
               "pow",
               "x^y",
               str("y", str(kSqrt, "x")),
               [&e] { e.binary(binary_op::power); },
               [&e] { e.binary(binary_op::root); }),
           un2("sqrt", str(kSqrt, "x"), unary_op::sqrt, str(kCubed, str(kSqrt, "x")), unary_op::cbrt),
           un("inv", "1/x", unary_op::reciprocal),
           un("abs", "|x|", unary_op::abs),
           digit(4),
           digit(5),
           digit(6),
           sub},
          {trig("sin", "sin", unary_op::sin, unary_op::asin, unary_op::sinh, unary_op::asinh),
           trig("cos", "cos", unary_op::cos, unary_op::acos, unary_op::cosh, unary_op::acosh),
           trig("tan", "tan", unary_op::tan, unary_op::atan, unary_op::tanh, unary_op::atanh),
           {"pi", kPiGlyph, [&e] { e.constant(kPi, kPiGlyph); }},
           {"e", "e", [&e] { e.constant(kE, "e"); }},
           digit(1),
           digit(2),
           digit(3),
           add},
          {un2("p10", "10^x", unary_op::pow10, "2^x", unary_op::pow2),
           second(
               "log", "log", "log_y x", [&e] { e.unary(unary_op::log10); }, [&e] { e.binary(binary_op::log_base); }),
           un2("ln", "ln", unary_op::ln, "e^x", unary_op::exp),
           un("floor", "floor", unary_op::floor),
           un("ceil", "ceil", unary_op::ceil),
           neg,
           digit(0),
           dot,
           eq},
      }};

  keypad_def programmer{
      "prog",
      72,
      42,
      {
          {bin("and", "AND", binary_op::bit_and),
           bin("or", "OR", binary_op::bit_or),
           digit(10),
           bin("lsh", "<<", binary_op::shift_left),
           bin("rsh", ">>", binary_op::shift_right),
           ce,
           bsp},
          {bin("nand", "NAND", binary_op::bit_nand),
           bin("nor", "NOR", binary_op::bit_nor),
           digit(11),
           lp,
           rp,
           mod,
           div},
          {bin("xor", "XOR", binary_op::bit_xor),
           un("not", "NOT", unary_op::bit_not),
           digit(12),
           digit(7),
           digit(8),
           digit(9),
           mul},
          {un("rol", "RoL", unary_op::rotate_left),
           un("ror", "RoR", unary_op::rotate_right),
           digit(13),
           digit(4),
           digit(5),
           digit(6),
           sub},
          {word("qword", calc::word_size::qword),
           word("dword", calc::word_size::dword),
           digit(14),
           digit(1),
           digit(2),
           digit(3),
           add},
          {word("word", calc::word_size::word), word("byte", calc::word_size::byte), digit(15), neg, digit(0), clr, eq},
      }};

  return {std::move(standard), std::move(scientific), std::move(programmer)};
}

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// Generated rather than a JSON literal: three keypads of up to 9 x 6 keys.
// Every child gets an explicit "order" so it renders in declaration order.
//
// Flags: NoResize, since fit_window() sizes the window to whichever keypad is
// visible. NoDocking is deliberate: docking would stretch the fixed key grid
// to fill a dock node, so the calculator stays a floating window even inside
// a desktop shell.

namespace {

/// A layout node under construction: its JSON members (without braces) and
/// named children, serialized in insertion order.
struct layout_node {
  std::string fields;
  std::vector<std::pair<std::string, layout_node>> children{};

  layout_node& add(const std::string& id, layout_node child) {
    children.emplace_back(id, std::move(child));
    return children.back().second;
  }
};

layout_node node(const std::string& type, const std::string& extra = "") {
  return {"\"type\":" + json_string(type) + (extra.empty() ? "" : "," + extra)};
}

std::string labelled(const std::string& label, const std::string& extra = "") {
  return "\"label\":" + json_string(label) + (extra.empty() ? "" : "," + extra);
}

std::string size_fields(int width, int height, int font_size) {
  return "\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
      ",\"font_size\":" + std::to_string(font_size);
}

layout_node dim_label(const std::string& text, int font_size) {
  return node(
      "Label",
      "\"text\":" + json_string(text) + ",\"font_size\":" + std::to_string(font_size) + ",\"text_color_light\":\"" +
          kDimLight + "\",\"text_color_dark\":\"" + kDimDark + "\"");
}

// A right-aligned Label: a Spring pushes it to the row's far edge.
layout_node right_aligned(const std::string& id, layout_node label) {
  layout_node row = node("HorizontalLayout");
  row.add("sp", node("Spring"));
  row.add(id, std::move(label));
  // A collapsed (weight 0) Spring: keeps one spacing of margin on the right.
  row.add("margin", node("Spring", "\"weight\":0"));
  return row;
}

std::string to_json(const layout_node& n, int order = -1) {
  std::string s = "{" + n.fields;
  if (order >= 0)
    s += ",\"order\":" + std::to_string(order);
  if (!n.children.empty()) {
    s += ",\"children\":{";
    for (std::size_t i = 0; i < n.children.size(); ++i) {
      if (i > 0)
        s += ",";
      s += json_string(n.children[i].first) + ":" + to_json(n.children[i].second, static_cast<int>(i));
    }
    s += "}";
  }
  return s + "}";
}

} // namespace

// Layouts get explicit "height" hints: a never-rendered widget (a keypad
// that starts hidden) has no natural size yet, so without them a keypad's
// first frames would be laid out from zero-height rows. The keypads and the
// radix panel get theirs only while visible (set_mode()): a hint reserves
// its height even on a hidden child.
static std::string spacing_and_height(int spacing, int height) {
  return "\"spacing\":" + std::to_string(spacing) + ",\"height\":" + std::to_string(height);
}

int bc::keypad_height(const keypad_def& pad) {
  int rows = static_cast<int>(pad.rows.size());
  return rows * pad.key_height + (rows - 1) * kKeySpacing;
}

std::string bc::make_layout(const std::vector<keypad_def>& pads) const {
  layout_node window = node("Window", "\"title\":\"Calculator\",\"flags\":\"NoResize|NoDocking\",\"closable\":true");

  // ── Menu bar ──
  layout_node& menu = window.add("menu", node("MenuBar"));
  layout_node& view = menu.add("view", node("Menu", labelled("View")));
  for (int m = 0; m < 3; ++m)
    view.add(kKeypadIds[m], node("MenuItem", labelled(kModeNames[m])));
  view.add("sep1", node("Separator"));
  view.add("deg", node("MenuItem", labelled("Degrees")));
  view.add("rad", node("MenuItem", labelled("Radians")));
  view.add("grad", node("MenuItem", labelled("Gradians")));
  view.add("sep2", node("Separator"));
  view.add("history", node("MenuItem", labelled("History")));
  view.add("grouping", node("MenuItem", labelled("Digit grouping")));
  layout_node& edit = menu.add("edit", node("Menu", labelled("Edit")));
  edit.add("copy", node("MenuItem", labelled("Copy")));
  edit.add("clear_history", node("MenuItem", labelled("Clear history")));

  // ── Calculator column ──
  layout_node& body = window.add("body", node("HorizontalLayout", "\"spacing\":" + std::to_string(kHistoryGap)));
  layout_node& main = body.add("main", node("VerticalLayout", "\"spacing\":4"));

  layout_node& tabs = main.add("tabs", node("HorizontalLayout", "\"spacing\":4,\"height\":24"));
  for (int m = 0; m < 3; ++m)
    tabs.add(kKeypadIds[m], node("Selectable", labelled(kModeNames[m], size_fields(kTabWidth, 24, 16))));

  main.add("expr_row", right_aligned("expr", dim_label(" ", 16)));
  main.add("display_row", right_aligned("display", node("Label", "\"text\":\"0\",\"font_size\":36")));
  main.add("status", dim_label(" ", 14));

  layout_node& radix =
      main.add("radix", node("VerticalLayout", "\"spacing\":" + std::to_string(kRadixSpacing) + ",\"visible\":false"));
  for (int r = 0; r < 4; ++r)
    radix.add(
        kRadixIds[r],
        node(
            "Selectable",
            labelled(
                kRadixNames[r],
                "\"width\":" + std::to_string(kRadixWidth) + ",\"height\":" + std::to_string(kRadixRowHeight) +
                    ",\"font_size\":16")));

  layout_node& mem = main.add("mem", node("HorizontalLayout", "\"spacing\":4,\"height\":26"));
  const char* mem_ids[] = {"mc", "mr", "mplus", "mminus", "ms"};
  const std::string mem_labels[] = {"MC", "MR", "M+", str("M", kMinus), "MS"};
  for (int i = 0; i < 5; ++i)
    mem.add(mem_ids[i], node("Button", labelled(mem_labels[i], size_fields(56, 26, 15))));

  for (const auto& pad : pads) {
    layout_node& keypad =
        main.add(pad.id, node("VerticalLayout", "\"spacing\":" + std::to_string(kKeySpacing) + ",\"visible\":false"));
    int font = pad.key_height >= 48 ? 22 : 17;
    for (std::size_t r = 0; r < pad.rows.size(); ++r) {
      layout_node& row = keypad.add(
          "r" + std::to_string(r), node("HorizontalLayout", spacing_and_height(kKeySpacing, pad.key_height)));
      for (const auto& k : pad.rows[r])
        row.add(k.id, node("Button", labelled(k.label, size_fields(pad.key_width, pad.key_height, font))));
    }
  }

  // ── History column ──
  layout_node& history = body.add("history", node("VerticalLayout", "\"spacing\":4,\"visible\":false"));
  history.add("title", node("Label", "\"text\":\"History\",\"font_size\":18"));
  history.add("empty", dim_label("There's no history yet", 14));
  for (std::size_t i = 0; i < kHistorySlots; ++i)
    history.add(
        "h" + std::to_string(i),
        node(
            "Selectable",
            labelled(
                "",
                "\"width\":" + std::to_string(kHistoryWidth) + ",\"height\":40,\"font_size\":15,\"visible\":false")));
  history.add(
      "clear",
      node(
          "Button",
          labelled("Clear history", "\"width\":" + std::to_string(kHistoryWidth) + ",\"height\":26,\"font_size\":15")));

  return to_json(window);
}

void bc::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__calc_");

  auto pads = make_keypads();
  std::string layout = make_layout(pads);
  for (std::size_t m = 0; m < pads.size() && m < keypad_sizes_.size(); ++m) {
    const auto& pad = pads[m];
    std::size_t columns = 0;
    for (const auto& row : pad.rows)
      columns = std::max(columns, row.size());
    int cols = static_cast<int>(columns);
    keypad_sizes_[m] = {cols * pad.key_width + (cols - 1) * kKeySpacing, keypad_height(pad)};
  }

  build_window(internal_root_key_, layout.c_str(), window_id_, [&](ui_tree& tree) {
    auto find = [&](const std::string& path) {
      ui_element_ptr out;
      tree.with(path, [&](const auto& el) { out = el; });
      return out;
    };
    auto bind = [&](const std::string& path, std::function<void()> handler) {
      ui_element_ptr el = find(path);
      if (el)
        on_click(wish_id_of(el), std::move(handler));
      return el;
    };

    window_ = find("");
    const std::string main = "body.main.";
    display_ = find(main + "display_row.display");
    expression_ = find(main + "expr_row.expr");
    status_ = find(main + "status");
    radix_panel_ = find(main + "radix");
    history_panel_ = find("body.history");
    history_empty_ = find("body.history.empty");
    copy_item_ = find("menu.edit.copy");

    // Mode tabs and View menu entries.
    for (int m = 0; m < 3; ++m) {
      auto mode = static_cast<calc::mode>(m);
      mode_tabs_[m] = bind(main + "tabs." + kKeypadIds[m], [this, mode] { set_mode(mode); });
      mode_items_[m] = bind(str("menu.view.", kKeypadIds[m]), [this, mode] { set_mode(mode); });
      keypads_[m] = find(main + kKeypadIds[m]);
    }
    const calc::angle_unit units[] = {calc::angle_unit::degrees, calc::angle_unit::radians, calc::angle_unit::gradians};
    const char* unit_ids[] = {"deg", "rad", "grad"};
    for (int u = 0; u < 3; ++u) {
      auto unit = units[u];
      angle_items_[u] = bind(str("menu.view.", unit_ids[u]), [this, unit] { engine_.set_angle_unit(unit); });
    }
    history_item_ = bind("menu.view.history", [this] {
      show_history_ = !show_history_;
      fit_window();
    });
    grouping_item_ = bind("menu.view.grouping", [this] { engine_.set_digit_grouping(!engine_.digit_grouping()); });
    bind("menu.edit.clear_history", [this] { engine_.clear_history(); });
    bind("body.history.clear", [this] { engine_.clear_history(); });

    // Programmer radix rows.
    for (int r = 0; r < 4; ++r) {
      int radix = kRadixes[r];
      radix_rows_[r] = bind(main + "radix." + kRadixIds[r], [this, radix] { engine_.set_radix(radix); });
    }

    // Memory keys.
    bind(main + "mem.mc", [this] { engine_.memory_clear(); });
    bind(main + "mem.mr", [this] { engine_.memory_recall(); });
    bind(main + "mem.mplus", [this] { engine_.memory_add(); });
    bind(main + "mem.mminus", [this] { engine_.memory_subtract(); });
    bind(main + "mem.ms", [this] { engine_.memory_store(); });

    // History slots.
    for (std::size_t i = 0; i < kHistorySlots; ++i)
      history_slots_.push_back(bind("body.history.h" + std::to_string(i), [this, i] { engine_.recall_history(i); }));

    // Keypads.
    for (const auto& pad : pads) {
      for (std::size_t r = 0; r < pad.rows.size(); ++r) {
        for (const auto& k : pad.rows[r]) {
          auto el = bind(main + pad.id + ".r" + std::to_string(r) + "." + k.id, k.action);
          if (el && k.label_fn)
            dynamic_labels_.emplace_back(el, k.label_fn);
        }
      }
    }
  });

  set_mode(calc::mode::standard);
  refresh();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void bc::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  // Window X button — forward to the client and clean up.
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  // Buttons and menu items emit "clicked", Selectables (mode tabs, radix
  // rows, history entries) emit "changed".
  if ((event == "clicked"_key || event == "changed"_key) && dispatch_click(id))
    refresh();
}

// ── State -> widgets ──────────────────────────────────────────────────────────

void bc::set_mode(calc::mode m) {
  engine_.set_mode(m);
  second_ = false;
  hyp_ = false;
  for (int i = 0; i < 3; ++i) {
    if (!keypads_[i])
      continue;
    bool active = i == static_cast<int>(m);
    (*keypads_[i])["visible"_key] = active;
    (*keypads_[i])["height"_key] = active ? static_cast<float>(keypad_sizes_[i].second) : 0.0f;
  }
  if (radix_panel_) {
    bool programmer = m == calc::mode::programmer;
    (*radix_panel_)["visible"_key] = programmer;
    (*radix_panel_)["height"_key] = programmer ? static_cast<float>(kRadixHeight) : 0.0f;
  }
  fit_window();
}

void bc::fit_window() {
  if (!window_)
    return;
  // Fixed parts of the layout, in pixels: window chrome above the tabs
  // (title bar, menu bar, padding), the rows between the tabs and the keypad
  // (tabs, expression, display, status, memory keys, spacing), the bottom
  // padding, and the programmer radix rows.
  constexpr int kChromeTop = 62;
  constexpr int kAboveKeypad = 138;
  constexpr int kBottom = 20;
  constexpr int kRadixRows = kRadixHeight + 4;
  constexpr int kSidePadding = 24;
  constexpr int kMinContentWidth = 3 * kTabWidth + 2 * kKeySpacing;

  auto m = static_cast<std::size_t>(engine_.get_mode());
  auto [pad_w, pad_h] = keypad_sizes_[m];
  bool programmer = engine_.get_mode() == calc::mode::programmer;
  int content_w = std::max({pad_w, kMinContentWidth, programmer ? kRadixWidth : 0});
  if (show_history_)
    content_w += kHistoryGap + kHistoryWidth;
  int height = kChromeTop + kAboveKeypad + pad_h + (programmer ? kRadixRows : 0) + kBottom;
  if (show_history_)
    height = std::max(height, kChromeTop + kHistoryHeight + kBottom);
  (*window_)["width"_key] = int32_t{content_w + kSidePadding};
  (*window_)["height"_key] = int32_t{height};
}

void bc::refresh() {
  const auto mode = engine_.get_mode();
  const bool programmer = mode == calc::mode::programmer;

  if (display_)
    (*display_)["text"_key] = engine_.display();
  if (expression_) {
    std::string expr = engine_.expression();
    (*expression_)["text"_key] = expr.empty() ? std::string{" "} : expr;
  }
  if (copy_item_)
    (*copy_item_)["copy_text"_key] = engine_.display_plain();

  // Status line: mode-specific settings plus memory / parenthesis markers.
  if (status_) {
    std::string status;
    if (programmer) {
      status = std::string{word_name(engine_.get_word_size())} + "  " +
          kRadixNames
              [engine_.radix() == 16       ? 0
                   : engine_.radix() == 10 ? 1
                   : engine_.radix() == 8  ? 2
                                           : 3];
    } else if (mode == calc::mode::scientific) {
      status = angle_name(engine_.get_angle_unit());
      if (engine_.scientific_notation())
        status += "  F-E";
    }
    if (engine_.has_memory())
      status += status.empty() ? "M" : "  M";
    if (engine_.open_parens() > 0)
      status += "  (" + std::to_string(engine_.open_parens());
    (*status_)["text"_key] = status.empty() ? std::string{" "} : status;
  }

  for (int m = 0; m < 3; ++m) {
    bool active = m == static_cast<int>(mode);
    if (mode_tabs_[m])
      (*mode_tabs_[m])["selected"_key] = active;
    if (mode_items_[m])
      (*mode_items_[m])["checked"_key] = active;
  }
  for (int u = 0; u < 3; ++u) {
    if (angle_items_[u])
      (*angle_items_[u])["checked"_key] = u == static_cast<int>(engine_.get_angle_unit());
  }
  if (history_item_)
    (*history_item_)["checked"_key] = show_history_;
  if (grouping_item_)
    (*grouping_item_)["checked"_key] = engine_.digit_grouping();

  if (programmer) {
    for (int r = 0; r < 4; ++r) {
      if (!radix_rows_[r])
        continue;
      std::string name = kRadixNames[r];
      (*radix_rows_[r])["label"_key] = name + std::string(6 - name.size(), ' ') + engine_.display_in(kRadixes[r]);
      (*radix_rows_[r])["selected"_key] = engine_.radix() == kRadixes[r];
    }
  }

  for (auto& [el, label_fn] : dynamic_labels_)
    (*el)["label"_key] = label_fn();

  // History panel.
  if (history_panel_)
    (*history_panel_)["visible"_key] = show_history_;
  const auto& history = engine_.history();
  if (history_empty_)
    (*history_empty_)["visible"_key] = history.empty();
  for (std::size_t i = 0; i < history_slots_.size(); ++i) {
    auto& slot = history_slots_[i];
    if (!slot)
      continue;
    bool used = i < history.size();
    (*slot)["visible"_key] = used;
    (*slot)["selected"_key] = false;
    if (used)
      (*slot)["label"_key] = history[i].expression + "\n" + history[i].result;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_bc() {
  auto proto = dynamic_ptr{"Bc"_key, {}};

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Bc"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Self-contained calculator with standard, scientific and programmer layouts, "
                        "memory and history. All arithmetic is server-side. "
                        "Listen for the 'closed' event to detect when the user is done."));

  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<bc>("wish"_key, "Bc"_key));
}

} // namespace bdg::wish
