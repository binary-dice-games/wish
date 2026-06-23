// MIT License © 2025 Binary Dice Games
/**
 * @file config_panel.cpp
 * @brief config_panel — wish C ABI example.
 *
 * Connects to a running wish server and renders a simple application settings
 * panel.  Uses only the wish C ABI (wish_client.h); all C++ is for
 * convenience (raw strings, std::cout, gflags).
 *
 * Usage:
 *   config_panel [OPTIONS]
 *
 * Quick start (socket mode):
 *   wish --port 7070 &
 *   config_panel --address 127.0.0.1:7070
 *
 * PTY mode (Linux — run inside the shell spawned by wish):
 *   wish --pty
 *   # in the bash session:
 *   config_panel --transport pty
 */
#include <wish/wish_client.h>

#include <gflags/gflags.h>

#include <iostream>

DEFINE_string(transport, "socket",
              "Transport type: socket, pipe, or pty");
DEFINE_string(address,   "127.0.0.1:7070",
              "Server address — host:port for socket, path for pipe");

// ── UI descriptor ─────────────────────────────────────────────────────────────

static const char* kDesc = R"({
  "type": "Window", "title": "Config Panel",
  "width": 420, "height": 300,
  "children": {
    "hdr":    { "type": "SeparatorText",
                "label": "Application Settings" },
    "s_spd":  { "type": "SliderInt",
                "label": "Render Speed",
                "value": 60, "min": 1, "max": 240 },
    "s_qual": { "type": "SliderFloat",
                "label": "Quality",
                "value": 0.75, "min": 0.0, "max": 1.0 },
    "t_name": { "type": "InputText",
                "label": "Profile",
                "value": "default" },
    "sep":    { "type": "Separator" },
    "btns": {
      "type": "HorizontalLayout", "spacing": 8,
      "children": {
        "apply": { "type": "Button", "label": "Apply", "width": 100 },
        "reset": { "type": "Button", "label": "Reset", "width": 100 },
        "quit":  { "type": "Button", "label": "Quit",  "width": 100 }
      }
    },
    "status": { "type": "Label", "text": "Ready." }
  }
})";

// ── Shared session state ──────────────────────────────────────────────────────
// Event handlers are invoked on the RMI worker thread and only call
// thread-safe ABI functions (wish_proxy_set_string, wish_client_quit).

static wish_client_t g_client = nullptr;
static wish_proxy_t  g_status = nullptr;

// ── Event handlers ────────────────────────────────────────────────────────────

static void on_apply(wish_proxy_t, wish_hash, void*) {
  std::cout << "[config_panel] Apply clicked\n";
  wish_proxy_set_string(g_status, wish_key("text"), "Settings applied.");
}

static void on_reset(wish_proxy_t, wish_hash, void*) {
  std::cout << "[config_panel] Reset clicked\n";
  wish_proxy_set_string(g_status, wish_key("text"), "Reset to defaults.");
}

static void on_quit(wish_proxy_t, wish_hash, void*) {
  std::cout << "[config_panel] Quit clicked - closing session\n";
  wish_client_quit(g_client);
}

// ── Session callback ──────────────────────────────────────────────────────────

static void session(wish_client_t c, void*) {
  g_client = c;

  wish_set_style_preset(c, "dark");

  if (wish_register_template(c, "cfg", kDesc) != WISH_OK) {
    std::cerr << "[config_panel] register_template failed: "
              << wish_last_error(c) << '\n';
    return;
  }

  if (!wish_instantiate_template(c, "cfg")) {
    std::cerr << "[config_panel] instantiate_template failed: "
              << wish_last_error(c) << '\n';
    return;
  }

  g_status = wish_proxy_get(c, "status");
  if (!g_status) {
    std::cerr << "[config_panel] proxy 'status' not found: "
              << wish_last_error(c) << '\n';
    return;
  }

  if (auto* p = wish_proxy_get(c, "btns.apply"))
    wish_proxy_on_event(p, "clicked", on_apply, nullptr);
  if (auto* p = wish_proxy_get(c, "btns.reset"))
    wish_proxy_on_event(p, "clicked", on_reset, nullptr);
  if (auto* p = wish_proxy_get(c, "btns.quit"))
    wish_proxy_on_event(p, "clicked", on_quit, nullptr);

  std::cout << "[config_panel] ready - close the panel or click Quit to exit\n";
  wish_client_wait(c);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
      "config_panel — wish C ABI example\n"
      "  Connects to a wish server and renders a settings panel.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  wish_transport_t transport = WISH_TRANSPORT_SOCKET;
  if (FLAGS_transport == "pty") {
    transport = WISH_TRANSPORT_PTY;
  } else if (FLAGS_transport == "pipe") {
    transport = WISH_TRANSPORT_PIPE;
  } else if (FLAGS_transport != "socket") {
    std::cerr << "[config_panel] unknown transport: " << FLAGS_transport
              << " (supported: socket, pipe, pty)\n";
    return 1;
  }

  wish_client_t c = wish_client_create(transport, FLAGS_address.c_str());
  if (!c) {
    std::cerr << "[config_panel] wish_client_create failed\n";
    return 1;
  }

  wish_error rc = wish_client_run(c, session, nullptr);
  if (rc != WISH_OK)
    std::cerr << "[config_panel] session error: " << wish_last_error(c) << '\n';

  wish_client_destroy(c);
  return rc == WISH_OK ? 0 : 1;
}
