// MIT License © 2026 Binary Dice Games
/// @file test_nymph.cpp
/// @brief The Nymph form over an in-memory RMI connection: the silent
///        load/render path and the edit-mode windows.
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <context/context.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include "modules/bdg/dev/nymph/server/nymph_document.hpp"
#include "modules/bdg/dev/nymph/server/nymph_png_meta.hpp"

#include <fstream>
#include <memory>
#include <optional>
#include <string>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace nymph = bdg::wish::nymph;
using namespace bdg::bison::rmi::transport;

namespace {

const std::string kFormat = "image:\n"
                            "  width: 320\n"
                            "  height: 200\n"
                            "root:\n"
                            "  type: Plot\n"
                            "  title: Revenue vs cost\n"
                            "  children:\n"
                            "    - type: PlotLine\n"
                            "      label: revenue\n"
                            "      xs: $month\n"
                            "      ys: $revenue\n"
                            "    - type: PlotBars\n"
                            "      label: cost\n"
                            "      xs: $month\n"
                            "      ys: $cost\n";
const std::string kData = "month, revenue, cost\n1, 12, 9\n2, 15, 9.5\n3, 11, 10\n";
const std::string kExample = "Monthly revenue against cost.\n--\n" + kFormat + "--\n" + kData;

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

class NymphTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  /// Instantiates a Nymph form; `root_` becomes its root key.
  void open(bool silent, bool view = false) {
    dynamic params;
    params["silent"_key] = silent;
    params["view"_key] = view;
    proxy_.emplace(client_->instantiate("wish"_key, "Nymph"_key, std::move(params)).get());
    ASSERT_TRUE(proxy_->valid());
    ASSERT_NE(srv_->last_session, nullptr);
    root_.clear();
    for (const auto& [k, _] : objects())
      if (k.rfind("__nymph_", 0) == 0 && k.find('.') == std::string::npos && k.find('_', 8) == std::string::npos &&
          k > root_)
        root_ = k;
    ASSERT_FALSE(root_.empty());
  }

  wish::name_map& objects() { return srv_->last_session->ui_objects; }

  void seed(const std::string& name, const std::string& content) {
    std::ofstream out(srv_->last_session->resource_dir / name, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  std::string slurp(const std::string& name) {
    std::ifstream in(srv_->last_session->resource_dir / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
  }

  dynamic load(const std::string& path, const std::string& display_path = "") {
    dynamic args;
    args["path"_key] = path;
    if (!display_path.empty())
      args["display_path"_key] = display_path;
    return proxy_->call("load"_key, std::move(args)).get();
  }

  /// The message of the error `load(path)` raises; "" if it succeeds.
  std::string load_error(const std::string& path) {
    try {
      load(path);
    } catch (const std::exception& e) {
      return e.what();
    }
    return {};
  }

  /// Calls render() and returns the sandbox file the "rendered" event names.
  std::string render() {
    size_t since = srv_->events->mark();
    proxy_->call("render"_key, dynamic{}).get();
    auto payload = srv_->events->wait_for("rendered"_key, since, {}, std::chrono::milliseconds{10000});
    EXPECT_TRUE(payload.has_value()) << "no 'rendered' event";
    return payload ? payload->as<std::string>("path"_key) : std::string{};
  }

  // ── edit mode helpers ──

  wish::ui_element_ptr element(const std::string& key) {
    auto it = objects().find(key);
    return it == objects().end() ? wish::ui_element_ptr{} : it->second;
  }

  std::string text_of(const std::string& key) {
    auto e = element(key);
    return e ? e->as<std::string>("text"_key) : std::string{"<missing>"};
  }

  std::string banner() { return text_of(root_ + "_source.vbox.banner"); }

  bison::key_t id_of(const std::string& key) {
    auto e = element(key);
    return e ? e->as<bison::key_t>("__wish_id"_key) : bison::key_t{};
  }

  /// Delivers a widget event to the form the way the render loop would.
  void fire(const std::string& root_suffix, const std::string& widget, bison::key_t event) {
    const std::string root = root_ + root_suffix;
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id_of(widget.empty() ? root : root + "." + widget), event, dynamic{});
  }

  /// The sandbox file behind one of the three editors ("format", "data", "description").
  std::string editor_file(const std::string& part) {
    auto e = element(root_ + "_source.vbox.tabs." + part + "_tab." + part);
    return e ? e->as<std::string>("file_path"_key) : std::string{};
  }

  /// Overwrites an editor's file and reports it changed, as typing would.
  void edit(const std::string& part, const std::string& content) {
    seed(editor_file(part), content);
    fire("_source", "vbox.tabs." + part + "_tab." + part, "changed"_key);
  }

  bool window_registered(const std::string& suffix) {
    return srv_->last_session->top_level_objects.count(bison::key_t{root_ + suffix}) != 0;
  }

  /// Children of an element, in index order (indexed children only).
  std::vector<dynamic_ptr> indexed_children(const wish::ui_element_ptr& e) {
    std::vector<dynamic_ptr> out;
    auto* cf = e->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>() || !cf->as<dynamic_ptr>())
      return out;
    auto children = cf->as<dynamic_ptr>();
    for (size_t i = 0;; ++i) {
      auto* f = children->findField(bison::key_t{static_cast<uint32_t>(i)});
      if (!f || !f->is<dynamic_ptr>())
        break;
      out.push_back(f->as<dynamic_ptr>());
    }
    return out;
  }

  std::string close_dialog_root() {
    for (const auto& [k, _] : objects())
      if (k.rfind("__message_box_", 0) == 0 && k.find('.') == std::string::npos)
        return k;
    return {};
  }

  void click_close_dialog_button(int index) { // 0=Yes, 1=No, 2=Cancel
    std::string r = close_dialog_root();
    ASSERT_FALSE(r.empty()) << "no close-confirmation dialog is open";
    auto id = objects().at(r + ".buttons.btn" + std::to_string(index))->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{r});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, "clicked"_key, dynamic{});
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
};

} // namespace

// ── Silent mode ──────────────────────────────────────────────────────────────

TEST_F(NymphTest, SilentBuildsNoVisibleWindow) {
  open(/*silent=*/true);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto holder = element(root_);
  ASSERT_TRUE(holder);
  EXPECT_FALSE(holder->get_as<bool>("visible"_key, true));
  EXPECT_FALSE(window_registered("_source"));
  EXPECT_FALSE(window_registered("_preview"));
  EXPECT_FALSE(window_registered("_data"));
}

TEST_F(NymphTest, LoadAndRenderProduceANymphPng) {
  open(true);
  seed("in.nymph", kExample);
  load("in.nymph");
  std::string out = render();
  ASSERT_FALSE(out.empty());

  std::string png = slurp(out);
  ASSERT_TRUE(nymph::is_png(png));
  // IHDR width/height: big-endian at bytes 16 and 20.
  EXPECT_EQ(static_cast<uint8_t>(png[18]) << 8 | static_cast<uint8_t>(png[19]), 320);
  EXPECT_EQ(static_cast<uint8_t>(png[22]) << 8 | static_cast<uint8_t>(png[23]), 200);
  auto source = nymph::read_source(png);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(*source, kExample); // byte for byte
  EXPECT_NE(png.find("Monthly revenue against cost."), std::string::npos); // the Description chunk
}

TEST_F(NymphTest, AnImageIsItsOwnSource) {
  open(true);
  seed("in.nymph", kExample);
  load("in.nymph");
  std::string first = slurp(render());
  ASSERT_TRUE(nymph::is_png(first));

  seed("again.png", first);
  load("again.png");
  std::string second = slurp(render());
  EXPECT_EQ(second, first); // same source, same pixels, same file
}

TEST_F(NymphTest, SourceReturnsTheLoadedText) {
  open(true);
  seed("in.nymph", kExample);
  load("in.nymph");
  std::string png = slurp(render());
  seed("img.png", png);
  load("img.png");
  auto reply = proxy_->call("source"_key, dynamic{}).get();
  EXPECT_EQ(slurp(reply.as<std::string>("path"_key)), kExample);
}

TEST_F(NymphTest, SourceIsAvailableEvenWhenItNoLongerBinds) {
  open(true);
  const std::string broken = "d\n--\nroot:\n  type: NoSuchThing\n--\n";
  seed("in.nymph", broken);
  dynamic args;
  args["path"_key] = std::string{"in.nymph"};
  args["validate"_key] = false;
  proxy_->call("load"_key, std::move(args)).get();
  auto reply = proxy_->call("source"_key, dynamic{}).get();
  EXPECT_EQ(slurp(reply.as<std::string>("path"_key)), broken);
}

TEST_F(NymphTest, BadSourceThrowsWithItsPosition) {
  open(true);
  std::string bad = kExample;
  bad.replace(bad.find("$revenue"), 8, "$revenu");
  seed("bad.nymph", bad);
  std::string message = load_error("bad.nymph");
  EXPECT_NE(message.find("13:11: unknown column 'revenu'"), std::string::npos) << message;
}

TEST_F(NymphTest, DisallowedElementTypeIsRejected) {
  open(true);
  seed("evil.nymph", "d\n--\nroot:\n  type: Image\n  src: ../../etc/passwd\n--\n");
  std::string message = load_error("evil.nymph");
  EXPECT_NE(message.find("not allowed"), std::string::npos) << message;
}

TEST_F(NymphTest, PlainPngIsNotANymphImage) {
  open(true);
  nymph::image img;
  img.width = img.height = 4;
  img.rgba.assign(4 * 4 * 4, 200);
  std::string png = nymph::encode_png(img, "x", "");
  // Cut the nymph chunk back out: 33 bytes of signature + IHDR, then the chunk.
  uint32_t length = 0;
  for (int i = 0; i < 4; ++i)
    length = length << 8 | static_cast<uint8_t>(png[33 + i]);
  png.erase(33, 12 + length);
  seed("plain.png", png);
  EXPECT_NE(load_error("plain.png").find("not a nymph image"), std::string::npos);
}

TEST_F(NymphTest, RenderBeforeLoadThrows) {
  open(true);
  EXPECT_THROW(proxy_->call("render"_key, dynamic{}).get(), std::exception);
  EXPECT_THROW(proxy_->call("source"_key, dynamic{}).get(), std::exception);
}

TEST_F(NymphTest, PathOutsideTheSandboxIsRejected) {
  open(true);
  EXPECT_FALSE(load_error("../outside.nymph").empty());
  EXPECT_FALSE(load_error("/etc/hostname").empty());
  EXPECT_FALSE(load_error("missing.nymph").empty());
}

TEST_F(NymphTest, TwoFormsRenderIndependently) {
  open(true);
  seed("a.nymph", kExample);
  load("a.nymph");
  std::string a = slurp(render());

  std::string other = kExample;
  other.replace(other.find("Revenue vs cost"), 15, "Something else!");
  open(true); // a second form in the same session
  seed("b.nymph", other);
  load("b.nymph");
  std::string b = slurp(render());

  EXPECT_EQ(*nymph::read_source(a), kExample);
  EXPECT_EQ(*nymph::read_source(b), other);
  EXPECT_NE(a, b);
}

// ── Edit mode ────────────────────────────────────────────────────────────────

TEST_F(NymphTest, EditModeBuildsItsWindows) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph", "/local/chart.png");

  ASSERT_TRUE(window_registered("_source"));
  ASSERT_TRUE(window_registered("_preview"));
  ASSERT_TRUE(window_registered("_data"));
  EXPECT_EQ(banner(), "");
  EXPECT_EQ(text_of(root_ + "_source.vbox.toolbar.path_label"), "/local/chart.png");

  // Each part of the source sits in its own editor file.
  EXPECT_EQ(slurp(editor_file("description")), "Monthly revenue against cost.\n");
  EXPECT_EQ(slurp(editor_file("format")), kFormat);
  EXPECT_EQ(slurp(editor_file("data")), kData);

  // The preview is the bound plot as real widgets, data included.
  auto figure = element(root_ + "_preview.figure");
  ASSERT_TRUE(figure);
  EXPECT_EQ(figure->findField(dynamic::CLASS)->as<bison::key_t>(), "Plot"_key);
  auto series = indexed_children(figure);
  ASSERT_EQ(series.size(), 2u);
  EXPECT_EQ(series[0]->findField(dynamic::CLASS)->as<bison::key_t>(), "PlotLine"_key);
  EXPECT_EQ(series[0]->findField("ys"_key)->as<std::vector<float>>().size(), 3u);
}

TEST_F(NymphTest, InvalidEditKeepsThePreviousPreview) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto before = element(root_ + "_preview.figure");
  ASSERT_TRUE(before);

  edit("format", "root:\n  type: Plot\n  title: [unclosed\n");
  EXPECT_NE(banner().find("Format, line"), std::string::npos) << banner();
  EXPECT_EQ(element(root_ + "_preview.figure"), before); // untouched, no flicker
  EXPECT_EQ(text_of(root_ + "_source.vbox.toolbar.path_label"), "(no file) [MODIFIED]");

  edit("format", kFormat);
  EXPECT_EQ(banner(), "");
  EXPECT_NE(element(root_ + "_preview.figure"), before); // rebuilt from the valid source
}

TEST_F(NymphTest, ErrorsAreLocatedWithinTheirOwnEditor) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  std::string format = kFormat;
  format.replace(format.find("$revenue"), 8, "$revenu");
  edit("format", format);
  EXPECT_EQ(banner().rfind("Format, line 11: unknown column 'revenu'", 0), 0u) << banner();

  edit("format", kFormat);
  edit("data", "month, revenue, cost\n1, 12, 9\n2, oops, 9.5\n");
  EXPECT_EQ(banner().rfind("Data, line 3: ", 0), 0u) << banner();
}

TEST_F(NymphTest, PreviewWindowKeepsItsIdAcrossEdits) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto before = id_of(root_ + "_preview");
  ASSERT_NE(before.id, 0u);

  std::string format = kFormat;
  format.replace(format.find("Revenue vs cost"), 15, "A new title");
  edit("format", format);
  ASSERT_EQ(banner(), "");
  EXPECT_EQ(id_of(root_ + "_preview"), before);
  EXPECT_EQ(element(root_ + "_preview.figure")->as<std::string>("title"_key), "A new title");
}

TEST_F(NymphTest, DataEditReachesTheTableAndThePlot) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto table = element(root_ + "_data.vbox.table");
  ASSERT_TRUE(table);
  EXPECT_EQ(table->as<int32_t>("columns"_key), 3);
  EXPECT_EQ(text_of(root_ + "_data.vbox.status"), "Showing 3 of 3 rows");
  auto data_window_id = id_of(root_ + "_data");

  edit("data", "month, revenue, cost\n1, 12, 9\n2, 99, 9.5\n");
  ASSERT_EQ(banner(), "");
  EXPECT_EQ(id_of(root_ + "_data"), data_window_id);
  EXPECT_EQ(text_of(root_ + "_data.vbox.status"), "Showing 2 of 2 rows");

  // Table children: 3 TableColumns, then one TableRow per data row.
  auto rows = indexed_children(element(root_ + "_data.vbox.table"));
  ASSERT_EQ(rows.size(), 3u + 2u);
  auto* cells = rows[4]->findField("children"_key);
  ASSERT_NE(cells, nullptr);
  auto revenue_cell = cells->as<dynamic_ptr>()->findField(bison::key_t{1U})->as<dynamic_ptr>();
  EXPECT_EQ(revenue_cell->as<std::string>("text"_key), "99");

  auto series = indexed_children(element(root_ + "_preview.figure"));
  ASSERT_EQ(series.size(), 2u);
  const auto& ys = series[0]->findField("ys"_key)->as<std::vector<float>>();
  ASSERT_EQ(ys.size(), 2u);
  EXPECT_FLOAT_EQ(ys[1], 99.0f);
}

TEST_F(NymphTest, DataWindowShowsAtMostAThousandRows) {
  open(false);
  std::string data = "month, revenue, cost\n";
  for (int i = 0; i < 5000; ++i)
    data += std::to_string(i) + ", 1, 2\n";
  seed("big.nymph", "d\n--\n" + kFormat + "--\n" + data);
  load("big.nymph");
  ASSERT_EQ(banner(), "");
  EXPECT_EQ(text_of(root_ + "_data.vbox.status"), "Showing 1000 of 5000 rows");
  EXPECT_EQ(indexed_children(element(root_ + "_data.vbox.table")).size(), 3u + 1000u);
  // The plot still gets every row.
  auto series = indexed_children(element(root_ + "_preview.figure"));
  EXPECT_EQ(series[0]->findField("ys"_key)->as<std::vector<float>>().size(), 5000u);
}

TEST_F(NymphTest, SaveWritesTheEditedSourceAndMatchesASilentRender) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph", "/local/chart.png");
  std::string format = kFormat;
  format.replace(format.find("Revenue vs cost"), 15, "Edited in place");
  edit("format", format);
  ASSERT_EQ(banner(), "");
  const std::string edited = "Monthly revenue against cost.\n--\n" + format + "--\n" + kData;

  size_t since = srv_->events->mark();
  fire("_source", "vbox.toolbar.save", "clicked"_key);
  auto saved = srv_->events->wait_for("on_image_saved"_key, since);
  ASSERT_TRUE(saved.has_value());
  std::string png = slurp(saved->as<std::string>("path"_key));
  ASSERT_TRUE(nymph::is_png(png));
  EXPECT_EQ(*nymph::read_source(png), edited);

  // Still marked modified until the client confirms it stored the file.
  EXPECT_EQ(text_of(root_ + "_source.vbox.toolbar.path_label"), "/local/chart.png [MODIFIED]");
  proxy_->call("mark_saved"_key, dynamic{}).get();
  EXPECT_EQ(text_of(root_ + "_source.vbox.toolbar.path_label"), "/local/chart.png");

  // One rasterizer: a silent render of the same text is the same file.
  open(true);
  seed("edited.nymph", edited);
  load("edited.nymph");
  EXPECT_EQ(slurp(render()), png);
}

TEST_F(NymphTest, CtrlSInAnEditorSaves) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  size_t since = srv_->events->mark();
  fire("_source", "vbox.tabs.format_tab.format", "saved"_key);
  EXPECT_TRUE(srv_->events->wait_for("on_image_saved"_key, since).has_value());
}

TEST_F(NymphTest, SavingABrokenSourceIsRefused) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  edit("format", "root:\n  type: Image\n");
  size_t since = srv_->events->mark();
  fire("_source", "vbox.toolbar.save", "clicked"_key);
  auto failed = srv_->events->wait_for("render_failed"_key, since);
  ASSERT_TRUE(failed.has_value());
  EXPECT_FALSE(srv_->events->saw("on_image_saved"_key, since));
  EXPECT_EQ(banner().rfind("Not saved: ", 0), 0u) << banner();
}

TEST_F(NymphTest, BadSourceInEditModeIsShownNotThrown) {
  open(false);
  seed("bad.nymph", "no separators here\n");
  EXPECT_EQ(load_error("bad.nymph"), "");
  EXPECT_NE(banner().find("separated by two '--' lines"), std::string::npos) << banner();
  ASSERT_TRUE(window_registered("_source"));
  // The text is not lost: it is offered in the Description editor.
  EXPECT_EQ(slurp(editor_file("description")), "no separators here\n");
}

TEST_F(NymphTest, ClosingUnmodifiedClosesAtOnce) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  size_t since = srv_->events->mark();
  fire("_source", "", "closed"_key);
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
  EXPECT_FALSE(window_registered("_source"));
  EXPECT_FALSE(window_registered("_preview"));
  EXPECT_FALSE(window_registered("_data"));
  EXPECT_FALSE(element(root_));
}

TEST_F(NymphTest, ClosingModifiedAsksAndNoDiscards) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  edit("description", "A new description.\n");
  size_t since = srv_->events->mark();
  fire("_source", "", "closed"_key);
  ASSERT_FALSE(close_dialog_root().empty());
  EXPECT_TRUE(window_registered("_source"));

  click_close_dialog_button(1); // No
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
  EXPECT_FALSE(srv_->events->saw("on_image_saved"_key, since));
  EXPECT_FALSE(window_registered("_source"));
}

TEST_F(NymphTest, ClosingModifiedYesSavesThenClosesOnMarkSaved) {
  open(false);
  seed("in.nymph", kExample);
  load("in.nymph");
  edit("description", "A new description.\n");
  size_t since = srv_->events->mark();
  fire("_source", "", "closed"_key);
  ASSERT_FALSE(close_dialog_root().empty());

  click_close_dialog_button(0); // Yes
  auto saved = srv_->events->wait_for("on_image_saved"_key, since);
  ASSERT_TRUE(saved.has_value());
  EXPECT_NE(nymph::read_source(slurp(saved->as<std::string>("path"_key)))->find("A new description."),
      std::string::npos);
  EXPECT_TRUE(window_registered("_source")); // waits for the client

  proxy_->call("mark_saved"_key, dynamic{}).get();
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
  EXPECT_FALSE(window_registered("_source"));
}

// ── View mode ────────────────────────────────────────────────────────────────

TEST_F(NymphTest, ViewModeShowsOnlyPreviewAndData) {
  open(/*silent=*/false, /*view=*/true);
  seed("in.nymph", kExample);
  load("in.nymph");

  EXPECT_TRUE(window_registered("_preview"));
  EXPECT_TRUE(window_registered("_data"));
  EXPECT_FALSE(window_registered("_source")); // no editors, no Save
  for (const auto& [key, _] : objects())
    EXPECT_EQ(key.find("TextEditor"), std::string::npos);
  EXPECT_FALSE(element(root_ + "_source.vbox.toolbar.save"));

  // The same bound plot and table edit mode shows.
  auto figure = element(root_ + "_preview.figure");
  ASSERT_TRUE(figure);
  EXPECT_EQ(figure->findField(dynamic::CLASS)->as<bison::key_t>(), "Plot"_key);
  auto series = indexed_children(figure);
  ASSERT_EQ(series.size(), 2u);
  EXPECT_EQ(series[0]->findField("ys"_key)->as<std::vector<float>>().size(), 3u);
  EXPECT_EQ(text_of(root_ + "_data.vbox.status"), "Showing 3 of 3 rows");
  // The Preview window is the one the user closes.
  EXPECT_TRUE(element(root_ + "_preview")->get_as<bool>("closable"_key, false));
}

TEST_F(NymphTest, ViewModeOpensANymphPng) {
  open(true);
  seed("in.nymph", kExample);
  load("in.nymph");
  std::string png = slurp(render());

  open(false, true);
  seed("chart.png", png);
  load("chart.png");
  ASSERT_TRUE(window_registered("_preview"));
  EXPECT_EQ(element(root_ + "_preview.figure")->as<std::string>("title"_key), "Revenue vs cost");
}

TEST_F(NymphTest, ViewModeRejectsABadSource) {
  open(false, true);
  std::string bad = kExample;
  bad.replace(bad.find("$revenue"), 8, "$revenu");
  seed("bad.nymph", bad);
  EXPECT_NE(load_error("bad.nymph").find("13:11: unknown column 'revenu'"), std::string::npos);
  EXPECT_FALSE(window_registered("_preview")); // nothing half-built
  EXPECT_FALSE(window_registered("_data"));
  seed("plain.txt", "no separators\n");
  EXPECT_FALSE(load_error("plain.txt").empty());
}

TEST_F(NymphTest, ViewModeIgnoresEventsThatWouldEditOrSave) {
  open(false, true);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto before = element(root_ + "_preview.figure");
  size_t since = srv_->events->mark();
  // Whatever arrives on the preview root, nothing is saved or rebuilt.
  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_ + "_preview"});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(id_of(root_ + "_preview.figure"), "changed"_key, dynamic{});
  h->second->on_event(id_of(root_ + "_preview.figure"), "saved"_key, dynamic{});
  h->second->on_event(id_of(root_ + "_preview.figure"), "clicked"_key, dynamic{});
  EXPECT_EQ(element(root_ + "_preview.figure"), before);
  EXPECT_FALSE(srv_->events->saw("on_image_saved"_key, since));
  EXPECT_EQ(slurp("nymph_out_0.png"), ""); // no image was written
}

TEST_F(NymphTest, ViewModeReloadKeepsTheWindows) {
  open(false, true);
  seed("in.nymph", kExample);
  load("in.nymph");
  auto preview_id = id_of(root_ + "_preview");
  std::string other = kExample;
  other.replace(other.find("Revenue vs cost"), 15, "Another chart!!");
  seed("other.nymph", other);
  load("other.nymph");
  EXPECT_EQ(id_of(root_ + "_preview"), preview_id);
  EXPECT_EQ(element(root_ + "_preview.figure")->as<std::string>("title"_key), "Another chart!!");
}

TEST_F(NymphTest, ClosingTheViewEndsIt) {
  open(false, true);
  seed("in.nymph", kExample);
  load("in.nymph");
  size_t since = srv_->events->mark();
  fire("_preview", "", "closed"_key);
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
  EXPECT_FALSE(window_registered("_preview"));
  EXPECT_FALSE(window_registered("_data"));
  EXPECT_FALSE(element(root_));
  EXPECT_TRUE(close_dialog_root().empty()); // never asks: nothing can be unsaved
}
