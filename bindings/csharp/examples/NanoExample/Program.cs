// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief Nano example using the wish C# binding.
 *
 * Port of modules/bdg/desktop/nano/client/nano.cpp and
 * bindings/python/examples/nano_example.py: the nano form
 * (server-side) never touches this process's local filesystem -- it only
 * edits files already sitting in its session sandbox. This program is the
 * bridge: it reacts to the form's high-level events by driving its own
 * local files through Client.UploadFile() / Client.DownloadFile().
 *
 * Start a wish server first (it owns the window/renderer), then point this
 * program at it:
 *
 *   build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
 *   dotnet run --project bindings/csharp/examples/NanoExample -- --transport=tcp --host=127.0.0.1 --port=7070 -- path/to/file
 *
 * Run with:  dotnet run --project bindings/csharp/examples/NanoExample -- [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH] [--theme=dark|light|classic] [file]
 */

using System.Text.Json;
using Bdg.Bison;
using Bdg.Bison.Rmi;
using Bdg.Wish;
using Client = Bdg.Wish.Client;

static List<Dictionary<string, object>> ListDirectory(string directory)
{
    // Mirrors list_directory() in modules/bdg/desktop/nano/client/nano.cpp: builds
    // the `files` list expected by FileDialog from a directory listing.
    var files = new List<Dictionary<string, object>>();
    var info = new DirectoryInfo(directory);
    if (info.Parent is not null)
    {
        files.Add(new Dictionary<string, object> { ["name"] = "..", ["type"] = "dir" });
    }
    try
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(directory).OrderBy(p => p, StringComparer.Ordinal))
        {
            files.Add(new Dictionary<string, object>
            {
                ["name"] = Path.GetFileName(entry),
                ["type"] = Directory.Exists(entry) ? "dir" : "file",
            });
        }
    }
    catch (IOException)
    {
    }
    catch (UnauthorizedAccessException)
    {
    }
    return files;
}

/// <summary>
/// Builds a Dynamic array of objects from a list of JSON-serializable
/// dictionaries. The C ABI has no way to set an object/array element at an
/// array index (there is no bison_set_object_at()), so nested arrays of
/// objects -- like FileDialog's `files` listing -- are built by
/// round-tripping through JSON instead. Dynamic.FromJson() only accepts a
/// top-level object, so the list is wrapped in one and the "items" field
/// (itself array-like) is projected back out.
/// </summary>
static Dynamic JsonArrayDynamic(List<Dictionary<string, object>> items)
{
    var json = JsonSerializer.Serialize(new Dictionary<string, object> { ["items"] = items });
    using var wrapper = Dynamic.FromJson(json);
    return (Dynamic)wrapper["items"]!; // independently ref-counted; survives wrapper's disposal
}

static void UploadAndOpen(Client client, Proxy nano, SandboxFiles files, string localPath)
{
    // Uploads a local file's current contents into the sandbox under a
    // fresh name and registers it as a new nano tab. Shared by the
    // FileDialog-driven Open/New flows and by opening a file passed on the
    // command line.
    var data = File.ReadAllBytes(localPath);
    var sandboxName = files.ReserveName(localPath);
    client.UploadFile(sandboxName, data);
    files.LocalPathBySandboxName[sandboxName] = localPath;
    using var result = nano.Call("open_file", new Dictionary<string, object?> { ["path"] = sandboxName, ["title"] = Path.GetFileName(localPath) });
}

static void BrowseAndOpen(Client client, Proxy nano, SandboxFiles files, string title, string confirmLabel, bool createIfMissing)
{
    // Shared by "Open" and "New": shows a FileDialog populated from a local
    // directory listing, then uploads the chosen file and registers it via
    // open_file(). `createIfMissing` is set for "New", where the chosen
    // path need not already exist locally.
    var curDir = Directory.GetCurrentDirectory();

    Dynamic MakeInit(string directory)
    {
        var init = new Dynamic();
        init["title"] = title;
        init["confirm_label"] = confirmLabel;
        init["path"] = directory;
        using var filesArr = JsonArrayDynamic(ListDirectory(directory));
        init["files"] = filesArr;
        return init;
    }

    var dlg = client.Instantiate("FileDialog", "wish");
    using (var init = MakeInit(curDir))
    {
        dlg.Set(init);
    }

    dlg.OnEvent("on_navigate", payload =>
    {
        var name = (string)payload["name"]!;
        var kind = (string)payload["type"]!;
        curDir = kind == "path"
            ? name
            : name == ".." ? (Path.GetDirectoryName(curDir) ?? curDir) : Path.Combine(curDir, name);
        using var init = MakeInit(curDir);
        dlg.Set(init);
    });

    dlg.OnEvent("on_open", payload =>
    {
        var name = (string)payload["path"]!;
        var localPath = Path.IsPathRooted(name) ? name : Path.Combine(curDir, name);

        // "New": the user typed/picked a path that may not exist yet --
        // create it empty. If it already exists (e.g. they picked an
        // existing file by mistake), leave its content alone rather than
        // truncating it.
        if (createIfMissing && !File.Exists(localPath))
        {
            File.WriteAllBytes(localPath, Array.Empty<byte>());
        }

        UploadAndOpen(client, nano, files, localPath);
    });

    dlg.OnEvent("on_cancel", _ => { });
}

static void RunNano(Client client, string? startupPath)
{
    var nano = client.Instantiate("Nano", "wish");
    var files = new SandboxFiles();

    // "Open" clicked: the server has no view of the client's local files, so
    // it asks us to present our own picker.
    nano.OnEvent("on_request_open", _ => BrowseAndOpen(client, nano, files, "Open File", "Open", createIfMissing: false));

    // "New" clicked: same handshake, but the chosen path need not exist yet.
    nano.OnEvent("on_request_new", _ => BrowseAndOpen(client, nano, files, "New File", "New", createIfMissing: true));

    // A tab (or the whole window) closed: download this file one last time
    // and forget our local bookkeeping for it.
    nano.OnEvent("on_file_closed", payload =>
    {
        var path = (string)payload["path"]!;
        if (files.LocalPathBySandboxName.Remove(path, out var localPath))
        {
            File.WriteAllBytes(localPath, client.DownloadFile(path));
        }
    });

    // Ctrl+S inside a tab, or the "Save" button clicked for the active tab:
    // download that one file, keep it open.
    nano.OnEvent("on_file_saved", payload =>
    {
        var path = (string)payload["path"]!;
        if (files.LocalPathBySandboxName.TryGetValue(path, out var localPath))
        {
            File.WriteAllBytes(localPath, client.DownloadFile(path));
        }
    });

    // The window's title-bar X was clicked while one or more tabs still had
    // unsaved changes: the server held the close open and is asking whether
    // to save them first. Confirm via the built-in MessageBox form, then
    // report the answer back so the server can actually finish closing.
    // "Cancel" simply never calls confirm_close() -- the window was never
    // actually removed from top_level_objects while waiting on this prompt,
    // so leaving it unanswered leaves it open exactly as it was.
    nano.OnEvent("on_confirm_close", payload =>
    {
        using var paths = (Dynamic)payload["paths"]!;
        var count = 0;
        foreach (var _ in paths) count++;
        var message = count == 1
            ? "1 file has unsaved changes. Save it before closing?"
            : $"{count} files have unsaved changes. Save them before closing?";

        var mb = client.Instantiate("MessageBox", "wish", new Dictionary<string, object?>
        {
            ["title"] = "Unsaved Changes",
            ["message"] = message,
            ["icon"] = "question",
            ["buttons"] = "yes_no_cancel",
        });

        // `mb` is a local variable here; referencing it inside its own
        // OnEvent handler (mirroring `dlg` in BrowseAndOpen above) is what
        // keeps the proxy alive until the user actually answers -- without
        // that reference the GC could finalize (and so destroy) it first.
        mb.OnEvent("on_result", result =>
        {
            var button = (string)result["button"]!;
            if (button != "cancel")
            {
                var save = button == "yes";
                using var callResult = nano.Call("confirm_close", new Dictionary<string, object?> { ["save"] = save });
            }
            mb.Release();
        });
    });

    nano.OnEvent("closed", _ => client.Quit());

    // A file to open at startup may be passed as the trailing positional argument.
    if (startupPath is not null)
    {
        if (File.Exists(startupPath))
        {
            UploadAndOpen(client, nano, files, startupPath);
        }
        else
        {
            Console.Error.WriteLine($"[nano] no such file: {startupPath}");
        }
    }

    client.Wait();
    nano.Release();
}

var options = CliOptions.Parse(args);
var startupPath = options.File is not null ? Path.GetFullPath(options.File) : null;

Client client;
if (options.Transport == "tcp")
{
    Console.WriteLine($"[Client] connecting to {options.Host}:{options.Port} ...");
    client = Client.Tcp(options.Host, (ushort)options.Port);
}
else if (options.Transport == "pipe")
{
    Console.WriteLine($"[Client] connecting to pipe {options.Name} ...");
    client = Client.Pipe(options.Name);
}
else
{
    Console.WriteLine("[Client] connecting via inherited stdio (--transport=term) ...");
    client = Client.Term();
}

// client.Run() calls wish_client_run(), which invokes the session callback
// on the library's own RMI worker thread and blocks *this* call until it
// returns. Run it on a background thread so the main thread can catch
// Ctrl+C and call client.Quit() to unblock RunNano()'s Wait().
Exception? threadError = null;
var worker = new Thread(() =>
{
    try
    {
        client.Run(c =>
        {
            c.SetStylePreset(options.Theme);
            RunNano(c, startupPath);
        });
    }
    catch (Exception exc)
    {
        threadError = exc;
    }
});
worker.IsBackground = true;

Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    Console.WriteLine("\n[Client] Ctrl+C -- quitting ...");
    client.Quit();
};

worker.Start();
worker.Join();

if (threadError is not null)
{
    throw threadError;
}
Console.WriteLine("[Client] done.");

/// <summary>
/// Tracks the sandbox name &lt;-&gt; local path mapping for files this
/// client has uploaded, and picks a sandbox name that does not collide with
/// one already in use (e.g. two different directories each containing a
/// "notes.txt"). Re-opening the exact same local path twice is not
/// deduplicated here -- the server already no-ops a duplicate open_file
/// call for a given sandbox path, so at worst this produces two
/// independently-edited tabs backed by two sandbox copies of one file.
/// </summary>
internal sealed class SandboxFiles
{
    public readonly Dictionary<string, string> LocalPathBySandboxName = new();

    public string ReserveName(string localPath)
    {
        var name = Path.GetFileName(localPath);
        var stem = Path.GetFileNameWithoutExtension(localPath);
        var ext = Path.GetExtension(localPath);
        var candidate = name;
        var suffix = 1;
        while (LocalPathBySandboxName.ContainsKey(candidate))
        {
            candidate = $"{stem}_{suffix}{ext}";
            suffix++;
        }
        return candidate;
    }
}

/// <summary>--transport/--host/--port/--name/--theme/--verbose flag parsing, plus a trailing positional file path.</summary>
internal sealed record CliOptions(string Transport, string Host, int Port, string Name, string Theme, bool Verbose, string? File)
{
    public static CliOptions Parse(string[] args)
    {
        var transport = "term";
        var host = "127.0.0.1";
        var port = 7070;
        var name = "";
        var theme = "dark";
        var verbose = false;
        string? file = null;

        foreach (var arg in args)
        {
            if (arg.StartsWith("--transport=")) transport = arg["--transport=".Length..];
            else if (arg.StartsWith("--host=")) host = arg["--host=".Length..];
            else if (arg.StartsWith("--port=")) port = int.Parse(arg["--port=".Length..]);
            else if (arg.StartsWith("--name=")) name = arg["--name=".Length..];
            else if (arg.StartsWith("--theme=")) theme = arg["--theme=".Length..];
            else if (arg == "--verbose") verbose = true;
            else if (!arg.StartsWith("--")) file = arg;
        }
        return new CliOptions(transport, host, port, name, theme, verbose, file);
    }
}
