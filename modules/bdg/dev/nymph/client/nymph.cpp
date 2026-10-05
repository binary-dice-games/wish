// MIT License © 2026 Binary Dice Games
/// @file nymph.cpp
/// @brief Client-side runner for the nymph embedded app.
///
/// The Nymph form (server-side) never touches the client's local files --
/// it only reads and writes its session sandbox. This runner is the bridge:
/// it uploads the local input, and downloads the PNG the form produced and
/// stores it locally. All parsing, validation and rendering happen in the
/// form, so this file knows nothing about the source format.
///
/// `render` and `extract` are command line tools: they do their work, set
/// the exit code and end the session without showing anything. For no
/// window at all run them under `wish standalone --renderer none`.
#include "modules/bdg/dev/nymph/client/nymph.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

constexpr const char* kUsage = "usage: nymph render  <in> [-o <out.png>]   source text (or nymph PNG) -> PNG, no UI\n"
                               "       nymph edit    <in> [-o <out.png>]   open the editing UI\n"
                               "       nymph extract <in.png> [-o <out>]   print the source a PNG carries\n"
                               "  <in> may be '-' for render: the source is read from the console (needs -o).\n";

// What `edit` starts from when the input file does not exist yet.
constexpr const char* kTemplate = "Describe the chart here.\n"
                                  "--\n"
                                  "image:\n"
                                  "  width: 800\n"
                                  "  height: 450\n"
                                  "root:\n"
                                  "  type: Plot\n"
                                  "  title: Example\n"
                                  "  x_label: x\n"
                                  "  y_label: value\n"
                                  "  children:\n"
                                  "    - type: PlotLine\n"
                                  "      label: value\n"
                                  "      xs: $x\n"
                                  "      ys: $value\n"
                                  "--\n"
                                  "x, value\n"
                                  "0, 12\n"
                                  "1, 6\n"
                                  "2, 4\n"
                                  "3, 8\n";

// How long `render` waits for the form's "rendered" event.
constexpr std::chrono::seconds kRenderTimeout{120};

struct arguments {
  std::string command;
  std::string input;
  std::string output; ///< empty when -o was not given
};

arguments parse_arguments(const std::vector<std::string>& args) {
  arguments out;
  std::vector<std::string> positional;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-o" || args[i] == "--output") {
      if (i + 1 >= args.size())
        throw std::invalid_argument(args[i] + " needs a file name");
      out.output = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.size() != 2)
    throw std::invalid_argument("expected a command and one input file");
  out.command = positional[0];
  out.input = positional[1];
  if (out.command != "render" && out.command != "edit" && out.command != "extract")
    throw std::invalid_argument("unknown command '" + out.command + "'");
  if (out.input == "-" && out.command != "render")
    throw std::invalid_argument("'-' (console input) is only supported by render");
  if (out.input == "-" && out.output.empty())
    throw std::invalid_argument("reading from the console needs -o <out.png>");
  return out;
}

std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read the file");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

/// Writes through a temporary file and a rename, so a failure never leaves
/// a half-written image where a good one used to be.
void write_local_file(const fs::path& path, const std::string& data) {
  fs::path temporary = path;
  temporary += ".nymph-tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out)
      throw std::runtime_error("cannot write " + path.string());
  }
  std::error_code ec;
  fs::rename(temporary, path, ec);
  if (ec) {
    fs::remove(temporary, ec);
    throw std::runtime_error("cannot write " + path.string());
  }
}

std::string read_console(wish_app_host& s) {
  std::string text;
  std::string line;
  while (s.read_console_line(line)) {
    text += line;
    text += '\n';
  }
  return text;
}

/// The image `render`/`edit` writes when -o is not given: the input with its
/// extension replaced by .png (a PNG input is rewritten in place).
fs::path default_output(const std::string& input) {
  fs::path out{input};
  out.replace_extension(".png");
  return out;
}

/// "<file>:<line>:<column>: message" for a positioned error from the form,
/// "<file>: message" otherwise.
std::string describe(const std::string& file, const std::string& message) {
  const bool positioned = !message.empty() && std::isdigit(static_cast<unsigned char>(message[0]));
  return "nymph: " + file + (positioned ? ":" : ": ") + message;
}

void fail(wish_app_host& s, const std::string& text) {
  std::cerr << text << "\n";
  s.set_exit_code(1);
  s.signal_done();
}

using form_ptr = std::shared_ptr<rmi::proxy::dynamic>;

form_ptr open_form(wish_app_host& s, bool silent) {
  dynamic params;
  params["silent"_key] = silent;
  return std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Nymph"_key, std::move(params)).get());
}

void load(const form_ptr& form, const std::string& sandbox_name, const std::string& display, bool validate = true) {
  dynamic args;
  args["path"_key] = sandbox_name;
  args["display_path"_key] = display;
  args["validate"_key] = validate;
  form->call("load"_key, std::move(args)).get();
}

// ── render ───────────────────────────────────────────────────────────────────

void run_render(wish_app_host& s, const arguments& args) {
  const std::string shown = args.input == "-" ? std::string{"<console>"} : args.input;
  const fs::path output = args.output.empty() ? default_output(args.input) : fs::path{args.output};

  std::string input = args.input == "-" ? read_console(s) : read_local_file(args.input);
  s.upload_file("nymph_input", input).get();

  auto form = open_form(s, /*silent=*/true);
  // The form renders on the server's render thread and answers with an event.
  struct outcome {
    bool ok{false};
    std::string text; ///< sandbox path of the PNG, or the failure message
  };
  auto result = std::make_shared<std::promise<outcome>>();
  auto answer = [result](bool ok, const std::string& text) {
    try {
      result->set_value({ok, text});
    } catch (const std::future_error&) {
      // Already answered.
    }
  };
  form->onEvent("rendered"_key, [answer](dynamic p) { answer(true, p.as<std::string>("path"_key)); });
  form->onEvent("render_failed"_key, [answer](dynamic p) { answer(false, p.as<std::string>("message"_key)); });

  load(form, "nymph_input", shown);
  form->call("render"_key, dynamic{}).get();

  auto pending = result->get_future();
  if (pending.wait_for(kRenderTimeout) != std::future_status::ready)
    throw std::runtime_error("timed out waiting for the image");
  outcome done = pending.get();
  if (!done.ok)
    throw std::runtime_error(done.text);

  write_local_file(output, s.download_file(done.text).get());
  s.signal_done();
}

// ── extract ──────────────────────────────────────────────────────────────────

void run_extract(wish_app_host& s, const arguments& args) {
  s.upload_file("nymph_input", read_local_file(args.input)).get();
  auto form = open_form(s, /*silent=*/true);
  // Not validated: a source that no longer renders can still be extracted.
  load(form, "nymph_input", args.input, /*validate=*/false);
  auto reply = form->call("source"_key, dynamic{}).get();
  std::string source = s.download_file(reply.as<std::string>("path"_key)).get();
  if (args.output.empty())
    std::cout << source << std::flush;
  else
    write_local_file(args.output, source);
  s.signal_done();
}

// ── edit ─────────────────────────────────────────────────────────────────────

void run_edit(wish_app_host& s, const arguments& args) {
  const fs::path output = args.output.empty() ? default_output(args.input) : fs::path{args.output};
  std::error_code ec;
  std::string input = fs::exists(args.input, ec) ? read_local_file(args.input) : std::string{kTemplate};
  s.upload_file("nymph_input", input).get();

  auto form = open_form(s, /*silent=*/false);

  // Save / Ctrl+S, or "Yes" on the close dialog: fetch the PNG the form
  // rendered, store it, and tell the form it landed -- which clears its
  // modified mark and completes a pending close.
  form->onEvent("on_image_saved"_key, [&s, form, output](dynamic p) {
    try {
      write_local_file(output, s.download_file(p.as<std::string>("path"_key)).get());
      form->call("mark_saved"_key, dynamic{}).get();
    } catch (const std::exception& e) {
      std::cerr << "nymph: " << e.what() << "\n";
    }
  });
  form->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  load(form, "nymph_input", output.string());
  // `form` stays alive through the shared_ptr captured above; the host
  // blocks until signal_done().
}

} // namespace

void run_nymph(wish_app_host& s) {
  arguments args;
  try {
    args = parse_arguments(s.app_args());
  } catch (const std::invalid_argument& e) {
    fail(s, std::string("nymph: ") + e.what() + "\n" + kUsage);
    return;
  }

  try {
    if (args.command == "render")
      run_render(s, args);
    else if (args.command == "extract")
      run_extract(s, args);
    else
      run_edit(s, args);
  } catch (const std::exception& e) {
    fail(s, describe(args.input == "-" ? std::string{"<console>"} : args.input, e.what()));
  }
}

namespace {
struct nymph_app_registrar {
  nymph_app_registrar() {
    register_app({
        .name = "nymph",
        .organization = WISH_MODULE_BDG_DEV_NYMPH_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_NYMPH_COLLECTION,
        .description = "Chart from text: render a source (description, format YAML, data CSV) to a PNG "
                       "that carries its own source, or edit one with a live preview",
        .params = {{"command", "render (no UI), edit, or extract"},
                   {"input", "A nymph source text or a nymph PNG ('-' with render: read the console)"},
                   {"-o file", "Output file (default: the input with a .png extension)"}},
        .run = run_nymph,
    });
  }
};
const nymph_app_registrar nymph_app_registrar_instance;
} // namespace

} // namespace bdg::wish
