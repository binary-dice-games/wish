// MIT License © 2025 Binary Dice Games
/// @file context.hpp
/// @brief Per-client state container for an active wish session.
#pragma once

#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_sync.hpp"

#include "src/rmi/server/context.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bdg::wish {

class ui_root; // defined in <wish/ui_root.hpp>

class file_service;
using file_service_ptr = std::shared_ptr<file_service>;

class style_service;
using style_service_ptr = std::shared_ptr<style_service>;

class logger;
using logger_ptr = std::shared_ptr<logger>;

#ifdef WISH_AUTOMATION_ENABLED
class automation_service;
using automation_service_ptr = std::shared_ptr<automation_service>;
#endif

/// @brief Number of consecutive render passes `context::dirty` requests by
/// default whenever something marks a session dirty.
///
/// A single follow-up frame is not always enough: ImGui auto-fit sizing can
/// take a couple of frames to settle on new content, and a modal popup's
/// `BeginPopupModal` doesn't report closed until the frame *after*
/// `ImGui::CloseCurrentPopup()` was called (see render_window()'s
/// "__request_close__" handling in imgui_ui_renderer.cpp). Every "mark
/// dirty" call site uses this same conservative constant rather than
/// picking its own frame count, so the render loop always renders a few
/// extra (cheap; skipped entirely once nothing is actually dirty) frames
/// past the minimum any single site happens to need today.
inline constexpr int32_t kDirtySettleFrames = 3;

/// @brief Holds all mutable state owned by one connected client.
///
/// A `bison::rmi::context` subclass: `session_id` and `emit_event` are
/// inherited directly, so wish never keeps its own copy of either. `objects`
/// is also inherited but is bison's *own* live RMI object table (keyed by
/// remote object id) -- an unrelated concept to this class's `ui_objects`
/// (the dot-path UI tree), so it is deliberately not shadowed/renamed.
///
/// Constructed when a client connects; destroyed (and `resource_dir` deleted)
/// when the client disconnects.  Not copyable; moveable so it can live in a
/// container that may rehash.
struct context : public bison::rmi::context {
  /// Flat map of dot-path name → `ui_element_ptr`.  The root node is at key
  /// `""`.  All named descendants follow the dot-joined ancestor naming
  /// convention (e.g. `"body.row.ok"`).  Stored as `ui_tree` so that
  /// imported trees can be merged in directly via `ui_objects.merge()`.
  wish::ui_tree ui_objects;

  /// Named UI template prototypes registered by the client: the fully
  /// resolved, typed `ui_element` tree root, built once at `register_template`
  /// time (see `ui_template::do_register`).  `instantiate_template` deep
  /// clones the stored root (`ui_element::clone_ptr()`) and assigns fresh RMI
  /// ids on every call rather than re-resolving element types each time.
  std::unordered_map<bison::key_t, ui_element_ptr, bison::key_t, bison::key_t> templates;

  /// Sandboxed temporary directory for this session's uploaded resources.
  /// Its `res/` subdirectory is pre-populated at construction time with
  /// read-only copies of the embedded assets (icons, fonts — see
  /// `resource_store::extract_to()`); a failed or partial extraction is
  /// non-fatal and does not throw.
  ///
  /// A `private/` subfolder is a reserved naming convention (not enforced by
  /// `file_service`, which sandboxes purely against `..`-escape): content an
  /// application places there — e.g. user-uploaded photos or other data that
  /// may be personal — is never offered to the browser's persistent
  /// resource cache (see `web_renderer`'s texture-check handshake in
  /// `src/web/DESIGN.md`), even though it is still cached server-side like
  /// any other resource.
  std::filesystem::path resource_dir;

  /// When `true`, the destructor does not delete `resource_dir` -- it is a
  /// persistent, identity-keyed directory outside the server's control, not
  /// this session's own throwaway temp dir. Only ever set by
  /// `server::on_authenticated()`; application code must not set it
  /// directly.
  bool resource_dir_persistent{false};

  /// CRC-32 of every embedded asset extracted into `resource_dir / "res"`,
  /// keyed by its path relative to `resource_dir` (e.g. `"res/icons/foo.png"`)
  /// so it can be looked up directly against a resolved `Image::src`. Reuses
  /// the zip's own per-file CRC-32 (already computed by miniz while
  /// unpacking the embedded archive) as a stable content-version number for
  /// the browser resource cache, so embedded assets never need a redundant
  /// CRC-32 pass over their own bytes at texture-load time.
  std::unordered_map<std::string, uint32_t> embedded_crc32s;

  /// Number of upcoming render passes the session still needs, not just
  /// whether it needs one: some state transitions only fully resolve after
  /// *several* consecutive frames (e.g. ImGui auto-fit sizing settling
  /// across two or three frames of new content, or a modal popup's
  /// `BeginPopupModal` not reporting closed until the frame after
  /// `ImGui::CloseCurrentPopup()` was called) — a plain "needs a redraw"
  /// bool can only guarantee the *next* frame renders, silently dropping
  /// whichever of those later frames nothing else happens to trigger. The
  /// render loop decrements this by one before each render (down to zero,
  /// never negative) instead of clearing it to zero outright, so a render
  /// function that bumps it back up mid-frame (see below) isn't stomped by
  /// that same frame's own decrement.
  ///
  /// Set (via `kDirtySettleFrames`, not a bare count, so every call site
  /// requests the same conservatively-safe number of follow-up frames) after
  /// any RMI dispatch touching this session and after any top-level event
  /// handler runs, so the render loop knows the session needs to be
  /// redrawn. Application code may also set it directly to force redraws
  /// after mutating session state from outside RMI dispatch (e.g. a
  /// background thread holding the session wlock directly), or from within
  /// a render function (which only ever sees a `const context&`) to request
  /// continuous redraws — e.g. a custom widget animating its own caret
  /// outside of ImGui's `WantTextInput` mechanism, or render_window()
  /// confirming a requested modal close. `mutable` for that last case;
  /// still an atomic since it's read from other threads via `rlock()`
  /// while dispatch holds the `wlock()`.
  mutable std::atomic<int32_t> dirty{kDirtySettleFrames};

  /// When `false` (default), widget file paths must be relative and are
  /// sandboxed inside `resource_dir`.  Set to `true` only for same-process
  /// deployments (memory_transport) where absolute host paths are safe.
  /// Controlled by `wish::server::set_allow_absolute_paths()`.
  bool allow_absolute_paths{false};

  /// When `false` (default), `http://`/`https://` widget file paths (e.g.
  /// `Image::src`, `Element.font_path`) are rejected outright -- no network
  /// request is ever made. Set to `true` to allow `file_service::resolve_or_fetch()`
  /// to download such URLs into the session sandbox. Controlled by
  /// `wish::server::set_allow_url_fetch()`.
  bool allow_url_fetch{false};

  /// File service instance; populated by `register_file_service(context&)`.
  file_service_ptr file_service;

  /// Style service instance; holds the client-configured ImGui theme fields.
  /// Read by the renderer before drawing this session's element tree.
  style_service_ptr style_service;

  /// Logger service instance; forwards client log calls to stdout / log file.
  logger_ptr logger_service;

#ifdef WISH_AUTOMATION_ENABLED
  /// Automation service instance; only set (by `server::on_session_created`/
  /// `standalone::on_session_created`) when the active renderer implements
  /// `automation::automation_backend` -- see `src/automation/DESIGN.md`'s
  /// "Native (ABI-based) automation" section. Null for renderers (e.g. the
  /// web renderer, which uses its own separate browser-based mechanism)
  /// that don't support it.
  automation_service_ptr automation_service;
#endif

  /// Map of key → root `ui_element_ptr` for every top-level window that the
  /// server must render each frame.  Both template instantiations and form
  /// objects register here:
  ///
  ///  - `ui_template::do_instantiate` adds the root at a unique key.
  ///  - `form::init()` adds the form's internal Window at `internal_root_key_`.
  ///  - `form::remove_internal_objects()` erases by that same key.
  ///
  /// All reads and writes are serialised by the session's own lock (see
  /// `sync_context`/`context_wlock`/`context_rlock` below): the render loop
  /// holds the read lock for the entire frame; every RMI dispatch holds the
  /// write lock via bison's `client_worker`, spanning the
  /// `on_before_dispatch` / `on_after_dispatch` hooks.
  std::unordered_map<bison::key_t, ui_element_ptr, bison::key_t, bison::key_t> top_level_objects;

  /// @brief One widget event queued during rendering; dispatched after the frame.
  struct pending_event {
    bison::key_t id; ///< `__wish_id` of the widget that fired
    bison::key_t event_name;
    bison::dynamic payload;
    bison::key_t root_key; ///< top_level_objects key at time of enqueue
  };

  /// @brief Events accumulated during one render frame; drained after the frame.
  ///
  /// The render loop moves these out while holding the session wlock, then
  /// delivers them (to the client and to `top_level_handlers`) after releasing
  /// the lock — preventing deadlocks and iterator-invalidation crashes.
  mutable std::vector<pending_event> pending_events;

  /// @brief Top-level key currently being rendered; set/cleared by the render loop.
  ///
  /// `enqueue_event()` copies this into `pending_event::root_key` so the
  /// dispatch phase can find the owning top-level handler without a map lookup.
  mutable bison::key_t current_top_level_key;

  /// @brief Maps top-level key → event handler (`ui_element` or `form`).
  ///
  /// Populated by `form::init()` and `ui_template` when they register a
  /// root window; cleared by `form::remove_internal_objects()` and template
  /// teardown.  The render loop snapshots this map before dispatching events.
  std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> top_level_handlers;

  /// @brief Set by `render_table()` around a `TableRow` cell's dispatch; read
  /// by `render_vertical_layout()`/`render_horizontal_layout()`.
  ///
  /// A `Table` row's hit-test is one `Selectable` spanning the whole row,
  /// with cell content overlaid on top via `ImGuiSelectableFlags_AllowOverlap`
  /// so it renders "on top without blocking input" (see render_table()'s own
  /// comment). That only holds as long as cell content never opens a real
  /// ImGui child window of its own: a nested `BeginChild()` is a distinct
  /// window that always wins hover/click priority over whatever's beneath it
  /// in the parent window, regardless of overlap flags, silently swallowing
  /// clicks that land on it before they ever reach the row's `Selectable`.
  /// `render_vertical_layout()`/`render_horizontal_layout()` normally wrap
  /// their own content in exactly such a `BeginChild()` (their `wrap_self`)
  /// for unrelated sizing/rect-report reasons; this flag suppresses that one
  /// `BeginChild()` for the whole subtree of a single cell dispatch, so a
  /// cell like `file_browser_utils.cpp`'s icon+label `HorizontalLayout`
  /// stays click-transparent to the row `Selectable` beneath it, same as a
  /// plain `Label` cell always has been.
  mutable bool suppress_layout_wrap_self{false};

  /// @brief Construct a session: creates a unique temporary directory.
  /// @param id  Session identifier; used to derive a unique directory name.
  explicit context(bison::key_t id);

  /// @brief Destroy the session: removes `resource_dir` and all its contents,
  ///        unless `resource_dir_persistent` is set.
  ~context();

  /// @brief (Re-)populate `resource_dir / "res"` with the embedded assets and
  ///        rebuild `embedded_crc32s`.
  ///
  /// Factored out of the constructor so `server::on_authenticated()` can run
  /// the same population logic again after switching `resource_dir` to a
  /// persistent, identity-keyed directory -- see `src/auth/DESIGN.md`. Only
  /// ever touches the `res/` subfolder, so re-running it against a directory
  /// that already has previously-uploaded files at its top level cannot
  /// clobber them. Failure is non-fatal (same contract as the constructor's
  /// original inline version): a client that can't see built-in icons/fonts
  /// is degraded, not fatal.
  void populate_resource_dir();

  context(const context&) = delete;
  context& operator=(const context&) = delete;
  context(context&& other) = delete;
  context& operator=(context&& other) = delete;
};

// ── Synchronized context wrapper ─────────────────────────────────────────────

/// Synchronized wrapper that owns a polymorphic `bison::rmi::context` and
/// serialises all access to it with a shared_mutex (multiple concurrent
/// readers or one exclusive writer).  The wrapped value is a `unique_ptr`
/// rather than a plain `context` because `bison::synchronized<T>` stores `T`
/// by value -- wrapping `context` directly would slice off `wish::context`'s
/// extra fields.  This is the exact type stored in
/// `bison::rmi::server::session_contexts()`, so wish shares the same
/// lockable slots rather than maintaining a second, parallel map.
using sync_context = bison::synchronized<std::unique_ptr<bison::rmi::context>>;
using sync_context_ptr = std::shared_ptr<sync_context>;

/// @brief RAII write-lock wrapper that exposes the underlying `wish::context`
///        directly (downcasting past the `unique_ptr<bison::rmi::context>`
///        indirection `sync_context` stores).
class context_wlock {
 public:
  explicit context_wlock(sync_context& sc) : lp_(sc.wlock()) {}

  wish::context& operator*() const {
    return static_cast<wish::context&>(**lp_);
  }
  wish::context* operator->() const {
    return &**this;
  }

 private:
  bison::locked_ptr<std::unique_ptr<bison::rmi::context>, std::unique_lock<std::shared_mutex>> lp_;
};

/// @brief Const/read-locked counterpart of `context_wlock`.
class context_rlock {
 public:
  explicit context_rlock(const sync_context& sc) : lp_(sc.rlock()) {}

  const wish::context& operator*() const {
    return static_cast<const wish::context&>(**lp_);
  }
  const wish::context* operator->() const {
    return &**this;
  }

 private:
  bison::locked_ptr<const std::unique_ptr<bison::rmi::context>, std::shared_lock<std::shared_mutex>> lp_;
};

namespace detail {
/// Thread-local pointer to the wish::context whose wlock is currently held by
/// bison's `client_worker` for the duration of an RMI dispatch.  Valid only
/// on the worker thread executing that dispatch; null at all other times.
///
/// Form and template-handler methods access session data through this pointer
/// rather than re-acquiring the lock (which would deadlock on a non-recursive
/// mutex).  Do NOT cache or dereference this outside the call stack that entered
/// dispatch.
extern thread_local context* current_context;

/// @brief Return the per-session singleton instance for a `__Wish*` protocol
///        class (`__WishFileSystem`, `__WishStyle`, `__WishLogger`).
///
/// These classes are session-scoped singletons rather than per-instantiate
/// objects, so `on_create_object` must return the existing service instance
/// instead of constructing a new one.
///
/// @param s     Session owning the singleton services.
/// @param klass Requested class key.
/// @return The singleton's `dynamic_ptr`, or an empty `dynamic_ptr` if
///         @p klass is not one of the singleton protocol classes (or the
///         session has no such service attached).
bison::dynamic_ptr find_singleton_service(const context& s, bison::key_t klass);

/// @brief Inject session context into a freshly created form/ui_template.
///
/// No-op if @p obj is null or not a `form`/`ui_template` instance.
///
/// @param obj       Object returned by the base class's `on_create_object`.
/// @param ctx       Per-session RMI context; must outlive @p obj.
/// @param sync_ctx  Synchronized wish session; held for @p obj's lifetime.
void init_session_object(const bison::dynamic_ptr& obj, bison::rmi::context& ctx, const sync_context_ptr& sync_ctx);

} // namespace detail

/// @brief Write a human-readable dump of every object in the session to @p out.
///
/// Outputs one line per entry in `context.ui_objects` and `context.top_level_objects`,
/// sorted by key, with the element's class type.  Useful for diagnosing missing
/// or unexpected elements in the UI tree.
///
/// Example output:
/// @code
/// wish session objects (6):
///   [DockSpaceViewport]  ""
///   [Window]             "demo_win"
///   [TabBar]             "demo_win.tabs_root"
///   top_level_objects (2):
///   [DockSpaceViewport]  "tpl_0"
///   [Window]             "__form_140703"
/// @endcode
void dump_session_tree(const context& s, std::ostream& out);

/// @brief Enqueue a widget event for deferred dispatch after the current frame.
///
/// Called from renderer code instead of `s.emit_event` directly.  The event
/// is appended to `s.pending_events` together with the current
/// `s.current_top_level_key` (set by the render loop before each
/// `render_session` call).  After the frame the render loop delivers every
/// queued event to the client and calls `on_event` on the owning
/// `ui_root`, preventing deadlocks and iterator-invalidation.
inline void enqueue_event(const context& s, bison::key_t id, bison::key_t event, bison::dynamic payload) {
  s.pending_events.push_back({id, event, std::move(payload), s.current_top_level_key});
}

} // namespace bdg::wish
