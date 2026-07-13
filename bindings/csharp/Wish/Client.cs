// MIT License © 2025 Binary Dice Games
/**
 * @file Client.cs
 * @brief RAII wrapper around `wish_client_handle`.
 *
 * Layered directly on the Bison C# binding: proxies and futures returned
 * here are plain `Bdg.Bison.Rmi.Proxy` / `Bdg.Bison.Rmi.Future` instances
 * (`Get`/`Set`/`Call`/`OnEvent` all work unchanged), since `libwish_client`
 * re-exports the bison and RMI C ABIs alongside its own -- see the module
 * comment in `Native.cs`.
 */

using System.Runtime.InteropServices;
using System.Text.Json.Serialization;
using Bdg.Bison;
using Bdg.Bison.Rmi;

namespace Bdg.Wish;

/// <summary>One parameter accepted by an embedded app (see <see cref="Client.ListApps"/>).</summary>
public sealed record AppParamInfo(
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("description")] string Description);

/// <summary>
/// One embedded app registered by an enabled optional module (see
/// <c>modules/README.md</c>). <see cref="Name"/> alone is what
/// <see cref="Client.RunApp"/> takes; <see cref="Organization"/> /
/// <see cref="Collection"/> are the module's location in the
/// <c>modules/&lt;organization&gt;/&lt;collection&gt;/&lt;name&gt;</c> tree
/// (empty if not populated).
/// </summary>
public sealed record AppInfo(
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("organization")] string Organization,
    [property: JsonPropertyName("collection")] string Collection,
    [property: JsonPropertyName("description")] string Description,
    [property: JsonPropertyName("params")] IReadOnlyList<AppParamInfo> Params);

/// <summary>
/// RAII wrapper around a <c>wish_client_handle</c>. Construct via
/// <see cref="Tcp"/>, <see cref="Stream"/>, <see cref="Pipe"/>, or
/// <see cref="Term"/>, then call <see cref="Run"/> to connect, drive the
/// session, and disconnect -- mirroring <c>wish_client_run()</c>: the
/// session callback runs on the library's RMI worker thread and the call
/// blocks until it returns (or a concurrent <see cref="Quit"/> unblocks a
/// <see cref="Wait"/> inside it).
/// </summary>
public sealed class Client : IDisposable
{
    private nint _handle;
    private object? _sessionCallback; // keeps the native-callback delegate alive during Run()

    private Client(nint handle) => _handle = handle;

    public static Client Tcp(string host, ushort port)
    {
        var h = Native.wish_client_tcp_create(host, port);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_client_tcp_create failed");
        }
        return new Client(h);
    }

    public static Client Stream(string path)
    {
        var h = Native.wish_client_stream_create(path);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_client_stream_create failed");
        }
        return new Client(h);
    }

    public static Client Pipe(string path)
    {
        var h = Native.wish_client_pipe_create(path);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_client_pipe_create failed");
        }
        return new Client(h);
    }

    public static Client Term()
    {
        var h = Native.wish_client_term_create();
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_client_term_create failed");
        }
        return new Client(h);
    }

    /// <summary>Frees the client. Must not be called while <see cref="Run"/> is active.</summary>
    public void Destroy()
    {
        if (_handle != nint.Zero)
        {
            Native.wish_client_destroy(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Destroy();
        GC.SuppressFinalize(this);
    }

    ~Client() => Destroy();

    public string LastError()
    {
        var ptr = Native.wish_last_error(_handle);
        return ptr == nint.Zero ? "" : Marshal.PtrToStringUTF8(ptr) ?? "";
    }

    // ── Session lifecycle ────────────────────────────────────────────────────

    /// <summary>
    /// Connects, invokes <paramref name="sessionFn"/>, then disconnects.
    /// Blocks until <paramref name="sessionFn"/> returns. It runs on the RMI
    /// worker thread; call <see cref="Wait"/> inside it to keep the session
    /// alive while event handlers update the UI, and end it with
    /// <see cref="Quit"/> (typically from an event handler).
    ///
    /// <paramref name="parameters"/> (a <see cref="Dynamic"/>, an
    /// <c>IDictionary&lt;string, object?&gt;</c>, or <c>null</c>) is
    /// forwarded to both the transport's connection setup and the server's
    /// connect handshake payload -- e.g. fields a server-side auth module
    /// inspects (see <c>src/auth/DESIGN.md</c>).
    /// </summary>
    public void Run(Action<Client> sessionFn, object? parameters = null)
    {
        Exception? caught = null;

        void Trampoline(nint _client, nint _userdata)
        {
            try
            {
                sessionFn(this);
            }
            catch (Exception exc) // C ABI boundary: cannot propagate directly
            {
                caught = exc;
            }
        }

        NativeSessionFn native = Trampoline;
        _sessionCallback = native;
        int rc;
        try
        {
            var fnPtr = Marshal.GetFunctionPointerForDelegate(native);
            using var scope = ParamsMarshal.From(parameters);
            rc = Native.wish_client_run_with_params(_handle, fnPtr, nint.Zero, scope.Handle);
        }
        finally
        {
            _sessionCallback = null;
        }
        if (caught is not null)
        {
            throw caught;
        }
        WishException.Check(rc, "run");
    }

    /// <summary>Blocks until <see cref="Quit"/> is called (from any thread).</summary>
    public void Wait() => Native.wish_client_wait(_handle);

    /// <summary>Signals the session to end; unblocks a concurrent <see cref="Wait"/>.</summary>
    public void Quit() => Native.wish_client_quit(_handle);

    // ── Style ─────────────────────────────────────────────────────────────────

    /// <summary>Applies a built-in style preset: "dark", "light", or "classic".</summary>
    public void SetStylePreset(string preset)
    {
        WishException.Check(Native.wish_set_style_preset(_handle, preset), "set_style_preset");
    }

    public Future SetStylePresetAsync(string preset)
    {
        WishException.Check(Native.wish_set_style_preset_async(_handle, preset, out var outFuture), "set_style_preset_async");
        return WishInterop.WrapFuture(outFuture);
    }

    // ── Template management ───────────────────────────────────────────────────

    /// <summary>Registers a named UI template (JSON or YAML descriptor string).</summary>
    public void RegisterTemplate(string name, string descriptor)
    {
        WishException.Check(Native.wish_register_template(_handle, name, descriptor), $"register_template({name})");
    }

    public Future RegisterTemplateAsync(string name, string descriptor)
    {
        WishException.Check(
            Native.wish_register_template_async(_handle, name, descriptor, out var outFuture),
            $"register_template_async({name})");
        return WishInterop.WrapFuture(outFuture);
    }

    /// <summary>Instantiates a registered template under dot-path <paramref name="prefix"/>
    /// and returns a <see cref="Proxy"/> to its root.</summary>
    public Proxy InstantiateTemplate(string name, string prefix)
    {
        var h = Native.wish_instantiate_template(_handle, name, prefix);
        if (h == nint.Zero)
        {
            throw new WishException(WishErrorCode.Exception, $"instantiate_template({name}, {prefix})");
        }
        return WishInterop.WrapProxy(h);
    }

    public Future InstantiateTemplateAsync(string name, string prefix)
    {
        WishException.Check(
            Native.wish_instantiate_template_async(_handle, name, prefix, out var outFuture),
            $"instantiate_template_async({name}, {prefix})");
        return WishInterop.WrapFuture(outFuture);
    }

    /// <summary>Resolves a dot-joined element path (see <see cref="InstantiateTemplate"/>)
    /// to a <see cref="Proxy"/>, from the client's local proxy map.</summary>
    public Proxy ProxyGet(string dotPath)
    {
        var h = Native.wish_proxy_get(_handle, dotPath);
        if (h == nint.Zero)
        {
            throw new WishException(WishErrorCode.NotFound, $"proxy_get({dotPath})");
        }
        return WishInterop.WrapProxy(h);
    }

    /// <summary>Releases every proxy cached under <paramref name="prefix"/> and its descendants.</summary>
    public void Release(string prefix)
    {
        WishException.Check(Native.wish_release(_handle, prefix), $"release({prefix})");
    }

    // ── Object instantiation ──────────────────────────────────────────────────

    /// <summary>
    /// Instantiates a remote object directly (no UI template involved).
    /// Unlike <see cref="InstantiateTemplate"/>, the result is not merged
    /// into the dot-path proxy map used by <see cref="ProxyGet"/>; the
    /// caller keeps and releases the returned proxy directly.
    /// </summary>
    public Proxy Instantiate(string klassName, string nsName = "", object? parameters = null)
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var klassKey = Key.Of(klassName);
        using var scope = ParamsMarshal.From(parameters);
        var h = Native.wish_instantiate(_handle, nsKey, klassKey, scope.Handle);
        if (h == nint.Zero)
        {
            throw new WishException(WishErrorCode.Exception, $"instantiate({klassName}, {nsName})");
        }
        return WishInterop.WrapProxy(h);
    }

    // ── Embedded apps ─────────────────────────────────────────────────────────

    /// <summary>
    /// Lists every embedded app registered by an enabled optional module
    /// (see <c>modules/README.md</c>). Mirrors <c>wish client --list</c>.
    /// Does not require a connection -- app registration happens at library
    /// load time, independent of any session.
    /// </summary>
    public static IReadOnlyList<AppInfo> ListApps()
    {
        WishException.Check(Native.wish_list_apps(out var outJson), "list_apps");
        try
        {
            var json = Marshal.PtrToStringUTF8(outJson) ?? "[]";
            return System.Text.Json.JsonSerializer.Deserialize<List<AppInfo>>(json) ?? [];
        }
        finally
        {
            WishInterop.FreeString(outJson);
        }
    }

    /// <summary>
    /// Connects, runs the named embedded app (see <see cref="ListApps"/>),
    /// blocks until it signals completion, then disconnects. Mirrors
    /// <c>wish client --run=&lt;name&gt; -- &lt;args...&gt;</c>.
    ///
    /// <paramref name="name"/> may be a short name (<c>"calculator"</c>) or
    /// its fully-qualified <c>"organization/collection/name"</c> form. Two
    /// different modules may register the same short name -- if
    /// <paramref name="name"/> is short and ambiguous between more than one,
    /// this throws a <see cref="WishException"/> with
    /// <see cref="WishErrorCode.Ambiguous"/>; use the fully-qualified name
    /// from <see cref="ListApps"/> instead.
    /// </summary>
    public void RunApp(string name, IReadOnlyList<string>? args = null)
    {
        var argv = args?.ToArray() ?? [];
        WishException.Check(Native.wish_run_app(_handle, name, argv, (nuint)argv.Length), $"run_app({name})");
    }

    // ── File transfer ──────────────────────────────────────────────────────────

    /// <summary>Uploads a file to the server's sandboxed session resource directory.</summary>
    public void UploadFile(string name, byte[] data)
    {
        WishException.Check(Native.wish_upload_file(_handle, name, data, (nuint)data.Length), $"upload_file({name})");
    }

    /// <summary>Downloads a previously uploaded file from the server.</summary>
    public byte[] DownloadFile(string name)
    {
        WishException.Check(Native.wish_download_file(_handle, name, out var outData, out var outLen), $"download_file({name})");
        try
        {
            var result = new byte[outLen];
            if (outLen > 0)
            {
                Marshal.Copy(outData, result, 0, (int)outLen);
            }
            return result;
        }
        finally
        {
            WishInterop.FreeString(outData);
        }
    }

    /// <summary>
    /// Uploads a file to the server, streaming it in chunks from a local
    /// file on disk instead of buffering the whole content in memory.
    /// </summary>
    public void UploadFileFromPath(string name, string localPath)
    {
        WishException.Check(
            Native.wish_upload_file_from_path(_handle, name, localPath), $"upload_file_from_path({name})");
    }

    /// <summary>
    /// Downloads a previously uploaded file, streaming it in chunks
    /// directly to a local file on disk instead of buffering the whole
    /// content in memory.
    /// </summary>
    public void DownloadFileToPath(string name, string localPath)
    {
        WishException.Check(
            Native.wish_download_file_to_path(_handle, name, localPath), $"download_file_to_path({name})");
    }

    /// <summary>
    /// Uploads a local zip archive and has the server unpack it into
    /// <paramref name="destPath"/> inside the sandbox.
    /// </summary>
    public void UploadPackageFromPath(string destPath, string localZipPath)
    {
        WishException.Check(
            Native.wish_upload_package_from_path(_handle, destPath, localZipPath),
            $"upload_package_from_path({destPath})");
    }

    // ── Logging ───────────────────────────────────────────────────────────────

    /// <summary>Sends a structured log message: <paramref name="level"/> is "debug"/"info"/"warn"/"error".</summary>
    public void Log(string level, string msg) => WishException.Check(Native.wish_log(_handle, level, msg), "log");

    public void LogDebug(string msg) => WishException.Check(Native.wish_log_debug(_handle, msg), "log_debug");

    public void LogInfo(string msg) => WishException.Check(Native.wish_log_info(_handle, msg), "log_info");

    public void LogWarn(string msg) => WishException.Check(Native.wish_log_warn(_handle, msg), "log_warn");

    public void LogError(string msg) => WishException.Check(Native.wish_log_error(_handle, msg), "log_error");
}
