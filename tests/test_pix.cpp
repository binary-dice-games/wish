// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace fs = std::filesystem;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class PixLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(PixLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "PixViewer"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "PixViewer"_key);
}

TEST_F(PixLocalTest, DefaultPathIsEmpty) {
  auto obj = dynamic::instantiate("wish"_key, "PixViewer"_key);
  auto* f = obj.findField<std::string>("path"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->empty());
}

// ── Session-capturing server for internal-tree tests ──────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

// Helper: find the root key for the internal form tree (starts with "__pix_",
// no dot -- i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__pix_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class PixWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
  }

  void TearDown() override {
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  std::string instantiate_and_get_root() {
    client_->instantiate("wish"_key, "PixViewer"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(PixWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __pix_... root key in session.objects";
}

TEST_F(PixWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(PixWindowTest, TreeContainsToolbarWidgets) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  EXPECT_TRUE(objs.count(root + ".vbox.toolbar.path_input"));
  EXPECT_TRUE(objs.count(root + ".vbox.toolbar.btn_browse"));
  EXPECT_TRUE(objs.count(root + ".vbox.toolbar.btn_open_explorer"));
}

TEST_F(PixWindowTest, TreeContainsGridAndPreviewTables) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  EXPECT_TRUE(objs.count(root + ".vbox.body.left_panel.grid_table"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.preview_table"));
}

TEST_F(PixWindowTest, TreeContainsInfoPanelLabels) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.info_panel.info_filename"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.info_panel.info_resolution"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.info_panel.info_format"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.info_panel.info_size"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.info_panel.info_modified"));
}

TEST_F(PixWindowTest, TreeContainsZoomControls) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.zoom_bar.btn_zoom_out"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.zoom_bar.btn_zoom_in"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.zoom_bar.btn_zoom_fit"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.zoom_bar.btn_zoom_100"));
  EXPECT_TRUE(objs.count(root + ".vbox.body.right_panel.zoom_bar.zoom_label"));
}

// ── Functional behavior (methods, events) ──────────────────────────────────────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class PixFunctionalTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "PixViewer"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());

    // Wrap emit_event to capture every high-level event emitted by form::emit().
    auto prev = std::move(srv_->last_session->emit_event);
    events_ = std::make_shared<std::vector<CapturedEvent>>();
    auto evts = events_;
    srv_->last_session->emit_event = [prev, evts](bison::key_t id, bison::key_t event, dynamic payload) {
      evts->push_back({event, payload});
      if (prev)
        prev(id, event, std::move(payload));
    };
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  bison::key_t wish_id_at(const std::string& path) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(path);
    if (it == objs.end() || !it->second)
      return {};
    return it->second->as<bison::key_t>("__wish_id"_key);
  }

  std::optional<std::string> text_at(const std::string& path) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(path);
    if (it == objs.end() || !it->second)
      return std::nullopt;
    auto* f = it->second->findField<std::string>("text"_key);
    if (!f)
      return std::nullopt;
    return *f;
  }

  void fire_event(const std::string& path, bison::key_t event, dynamic payload = {}) {
    auto id = wish_id_at(path);
    ASSERT_TRUE(id.id) << "no __wish_id at " << path;
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, payload);
  }

  bool has_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name == name)
        return true;
    return false;
  }

  // form::emit() (called from on_event(), which runs outside dispatch)
  // enqueues onto the session's event queue; delivery to emit_event -- and
  // thus to events_ -- only happens when the render loop's own thread next
  // flushes it, not synchronously with fire_event() returning. Spin briefly
  // for it, same idiom as test_nano.cpp's wait_for_event().
  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!has_event(name) && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has_event(name);
  }

  std::optional<dynamic> payload_of(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name == name)
        return e.payload;
    return std::nullopt;
  }

  // Dynamically-created grid cells (rows/cells/image/selectable, built by
  // pix_viewer::rebuild_grid()) are nested dynamic_ptr children reachable
  // only via each ancestor's own "children" field -- unlike the statically
  // imported layout, they are NOT separately registered as their own
  // dot-path entries in session.ui_objects (mirrors test_nano.cpp's
  // editor_at()/tab_id_at() helpers, which navigate the TabBar's own
  // "children" field the same way rather than assuming a deeper dot-path).
  dynamic_ptr child_at(const dynamic_ptr& parent, size_t index) const {
    if (!parent)
      return {};
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    auto& child_f = (**cf)[index];
    if (!child_f.is<dynamic_ptr>())
      return {};
    return child_f.as<dynamic_ptr>();
  }

  // Grid cell at (row, col): grid_table -> row -> cell, where the cell
  // itself is the Selectable (click target), wrapping {0: Image,
  // 1: Label (filename)} as overlay children -- see render_selectable()'s
  // children-overlay doc comment in imgui_ui_renderer.cpp. Returns null if
  // out of range or padding.
  dynamic_ptr grid_cell(size_t row, size_t col) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.body.left_panel.grid_table");
    if (it == objs.end())
      return {};
    return child_at(child_at(it->second, row), col);
  }

  dynamic set_images(std::vector<std::string> names) {
    dynamic list;
    size_t i = 0;
    for (auto& n : names) {
      auto e = std::make_shared<dynamic>();
      (*e)["name"_key] = n;
      list[i++] = dynamic_ptr{e};
    }
    dynamic args;
    args["images"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};
    return proxy_->call("set_images"_key, std::move(args)).get();
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(PixFunctionalTest, SetImagesBuildsGridWithPlaceholderThumbnails) {
  set_images({"a.png", "b.png"});
  // Row 0, col 0 -> a Selectable{0: Label(filename), 1: Image} -- caption
  // above the thumbnail so captions stay aligned across cells regardless
  // of each thumbnail's own aspect-fit height (see rebuild_grid()).
  auto cell = grid_cell(0, 0);
  ASSERT_TRUE(cell);
  auto name_label = child_at(cell, 0);
  auto image = child_at(cell, 1);
  ASSERT_TRUE(image);
  ASSERT_TRUE(name_label);

  auto* src = image->findField<std::string>("src"_key);
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(*src, "res/icons/image.png");
  auto* text = name_label->findField<std::string>("text"_key);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(*text, "a.png");
}

TEST_F(PixFunctionalTest, SetThumbnailUpdatesMatchingCell) {
  set_images({"a.png", "b.png"});
  dynamic args;
  args["name"_key] = std::string{"a.png"};
  args["thumb_path"_key] = std::string{"pix_cache/x/thumbs/a.png.png"};
  proxy_->call("set_thumbnail"_key, std::move(args)).get();

  auto image = child_at(grid_cell(0, 0), 1);
  ASSERT_TRUE(image);
  auto* src = image->findField<std::string>("src"_key);
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(*src, "pix_cache/x/thumbs/a.png.png");
}

TEST_F(PixFunctionalTest, SetThumbnailWithSizeFitsNonSquareAspectRatioWithoutStretching) {
  set_images({"a.png"});
  dynamic args;
  args["name"_key] = std::string{"a.png"};
  args["thumb_path"_key] = std::string{"pix_cache/x/thumbs/a.png.png"};
  // A 96x48 (2:1) source thumbnail must fit inside the 84x84 cell
  // preserving that ratio (84x42), not stretch back out to 84x84.
  args["width"_key] = int32_t{96};
  args["height"_key] = int32_t{48};
  proxy_->call("set_thumbnail"_key, std::move(args)).get();

  auto image = child_at(grid_cell(0, 0), 1);
  ASSERT_TRUE(image);
  auto* w = image->findField<int32_t>("width"_key);
  auto* h = image->findField<int32_t>("height"_key);
  ASSERT_NE(w, nullptr);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(*w, 84);
  EXPECT_EQ(*h, 42);
}

TEST_F(PixFunctionalTest, SetInfoPopulatesLabelsWithPrefixes) {
  dynamic args;
  args["filename"_key] = std::string{"photo.png"};
  args["resolution"_key] = std::string{"800 x 600"};
  args["format"_key] = std::string{"PNG"};
  args["size"_key] = std::string{"3.3 KB"};
  args["modified"_key] = std::string{"2026-01-01 00:00"};
  proxy_->call("set_info"_key, std::move(args)).get();

  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.info_panel.info_filename"), "photo.png");
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.info_panel.info_resolution"), "Resolution: 800 x 600");
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.info_panel.info_format"), "Format: PNG");
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.info_panel.info_size"), "Size: 3.3 KB");
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.info_panel.info_modified"), "Modified: 2026-01-01 00:00");
}

TEST_F(PixFunctionalTest, SetPreviewUpdatesImageAndZoomLabel) {
  dynamic args;
  args["loading"_key] = false;
  args["src"_key] = std::string{"pix_cache/x/full/photo.png"};
  args["width"_key] = int32_t{400};
  args["height"_key] = int32_t{300};
  args["zoom_percent"_key] = 50.0f;
  proxy_->call("set_preview"_key, std::move(args)).get();

  auto& objs = srv_->last_session->ui_objects;
  auto& img = objs.at(root_ + ".vbox.body.right_panel.preview_table.prow0.preview_image");
  EXPECT_EQ(*img->findField<std::string>("src"_key), "pix_cache/x/full/photo.png");
  EXPECT_EQ(*img->findField<int32_t>("width"_key), 400);
  EXPECT_EQ(*img->findField<int32_t>("height"_key), 300);
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.zoom_bar.zoom_label"), "50%");
}

TEST_F(PixFunctionalTest, SetPreviewWidensColumnToMatchZoomedImageWidth) {
  // preview_table's TableColumn has a fixed pixel width (ImGui doesn't
  // auto-expand a column to an oversized cell's content), so it must track
  // the image's own current (zoomed) width for ScrollX to pan across the
  // image's full extent instead of clipping it -- see do_set_preview().
  dynamic args;
  args["loading"_key] = false;
  args["src"_key] = std::string{"pix_cache/x/full/photo.png"};
  args["width"_key] = int32_t{1500};
  args["height"_key] = int32_t{900};
  args["zoom_percent"_key] = 100.0f;
  proxy_->call("set_preview"_key, std::move(args)).get();

  auto& objs = srv_->last_session->ui_objects;
  auto& col = objs.at(root_ + ".vbox.body.right_panel.preview_table.pcol0");
  EXPECT_EQ(*col->findField<float>("init_width"_key), 1500.0f);
}

TEST_F(PixFunctionalTest, SetPreviewLoadingShowsLoadingLabel) {
  dynamic args;
  args["loading"_key] = true;
  proxy_->call("set_preview"_key, std::move(args)).get();
  EXPECT_EQ(text_at(root_ + ".vbox.body.right_panel.zoom_bar.zoom_label"), "Loading...");
}

TEST_F(PixFunctionalTest, StatFilesReportsMissingAndExistingFiles) {
  // Seed a file directly into the session sandbox, as if already uploaded.
  std::ofstream out(srv_->last_session->resource_dir / "present.png", std::ios::binary);
  out << "fake png bytes";
  out.close();

  dynamic paths;
  paths[size_t{0}] = std::string{"present.png"};
  paths[size_t{1}] = std::string{"missing.png"};
  dynamic args;
  args["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};
  auto results = proxy_->call("stat_files"_key, std::move(args)).get();

  auto& present = *results[size_t{0}].as<dynamic_ptr>();
  EXPECT_TRUE(present.as<bool>("exists"_key));
  EXPECT_GT(present.as<int32_t>("mtime"_key), 0);

  auto& missing = *results[size_t{1}].as<dynamic_ptr>();
  EXPECT_FALSE(missing.as<bool>("exists"_key));
  EXPECT_EQ(missing.as<int32_t>("mtime"_key), 0);
}

TEST_F(PixFunctionalTest, StatFilesRejectsSandboxEscape) {
  dynamic paths;
  paths[size_t{0}] = std::string{"../../etc/passwd"};
  dynamic args;
  args["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};
  auto results = proxy_->call("stat_files"_key, std::move(args)).get();
  auto& entry = *results[size_t{0}].as<dynamic_ptr>();
  EXPECT_FALSE(entry.as<bool>("exists"_key));
}

TEST_F(PixFunctionalTest, DeleteFileRemovesUploadedFile) {
  auto path = srv_->last_session->resource_dir / "evict_me.png";
  std::ofstream out(path, std::ios::binary);
  out << "fake png bytes";
  out.close();
  ASSERT_TRUE(std::filesystem::exists(path));

  dynamic args;
  args["path"_key] = std::string{"evict_me.png"};
  proxy_->call("delete_file"_key, std::move(args)).get();

  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(PixFunctionalTest, DeleteFileIsNoOpForMissingOrSandboxEscapingPath) {
  dynamic args;
  args["path"_key] = std::string{"never_existed.png"};
  EXPECT_NO_THROW(proxy_->call("delete_file"_key, args.clone()).get());

  dynamic escape_args;
  escape_args["path"_key] = std::string{"../../etc/passwd"};
  EXPECT_NO_THROW(proxy_->call("delete_file"_key, std::move(escape_args)).get());
}

TEST_F(PixFunctionalTest, BrowseButtonEmitsEvent) {
  fire_event(root_ + ".vbox.toolbar.btn_browse", "clicked"_key);
  EXPECT_TRUE(wait_for_event("on_browse_clicked"_key));
}

TEST_F(PixFunctionalTest, PathInputChangedEmitsSubmittedAndUpdatesField) {
  dynamic payload;
  payload["value"_key] = std::string{"/home/user/pictures"};
  fire_event(root_ + ".vbox.toolbar.path_input", "changed"_key, payload);

  ASSERT_TRUE(wait_for_event("on_path_submitted"_key));
  auto p = payload_of("on_path_submitted"_key);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->as<std::string>("path"_key), "/home/user/pictures");

  auto path_field = proxy_->get().get();
  EXPECT_EQ(path_field.as<std::string>("path"_key), "/home/user/pictures");
}

TEST_F(PixFunctionalTest, ZoomButtonsEmitViewControlWithCorrectAction) {
  fire_event(root_ + ".vbox.body.right_panel.zoom_bar.btn_zoom_in", "clicked"_key);
  ASSERT_TRUE(wait_for_event("on_view_control"_key));
  auto p = payload_of("on_view_control"_key);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->as<std::string>("action"_key), "zoom_in");
}

TEST_F(PixFunctionalTest, SelectableClickEmitsImageSelectedWithName) {
  set_images({"a.png", "b.png", "c.png"});

  // Row 0, col 1 ("b.png"): a dynamically-created cell, not a statically
  // imported one -- fire the event directly against its own __wish_id
  // (see grid_cell()'s doc comment on why fire_event()'s dot-path lookup
  // doesn't reach it). The cell itself is the Selectable.
  auto selectable = grid_cell(0, 1);
  ASSERT_TRUE(selectable);
  auto sel_id = selectable->as<bison::key_t>("__wish_id"_key);
  ASSERT_TRUE(sel_id.id);

  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(sel_id, "changed"_key, dynamic{});

  ASSERT_TRUE(wait_for_event("on_image_selected"_key));
  auto p = payload_of("on_image_selected"_key);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->as<std::string>("name"_key), "b.png");
  EXPECT_TRUE(*selectable->findField<bool>("selected"_key));
}

TEST_F(PixFunctionalTest, WindowClosedEmitsClosedAndRemovesTree) {
  auto win_id = wish_id_at(root_);
  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(win_id, "closed"_key, dynamic{});

  EXPECT_TRUE(wait_for_event("closed"_key));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_));
}
