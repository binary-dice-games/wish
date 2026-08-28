// MIT License © 2025 Binary Dice Games
/**
 * @file Server.cs
 * @brief RAII wrapper around `wish_server_handle` -- hosts and renders a
 *        real wish session, over `libwish_server` (a separate shared
 *        library from `libwish_client`; see `ServerNative.cs`'s doc
 *        comment).
 */

using System.Runtime.InteropServices;

namespace Bdg.Wish;

/// <summary>
/// Converts a flat <c>IDictionary&lt;string, object&gt;</c> of <c>string</c>,
/// <c>bool</c>, <c>int</c>, or <c>float</c> values into a raw
/// <c>bison_handle</c>, using <see cref="ServerNative"/>'s own embedded
/// <c>bison_*</c> functions (not <c>Bdg.Bison.Native</c> -- see
/// <c>ServerNative.cs</c>'s doc comment for why). Everything
/// <see cref="Server.Start"/>'s renderer/listen params need.
/// </summary>
internal readonly struct ServerParamsScope : IDisposable
{
    public nint Handle { get; }

    public ServerParamsScope(nint handle) => Handle = handle;

    public void Dispose()
    {
        if (Handle != nint.Zero)
        {
            ServerNative.bison_release(Handle);
        }
    }

    public static ServerParamsScope From(IReadOnlyDictionary<string, object>? fields)
    {
        if (fields is null || fields.Count == 0)
        {
            return new ServerParamsScope(nint.Zero);
        }

        var handle = ServerNative.bison_create(0);
        if (handle == nint.Zero)
        {
            throw new OutOfMemoryException("bison_create failed");
        }

        foreach (var (name, value) in fields)
        {
            var key = ServerNative.bison_key(name);
            int rc = value switch
            {
                bool b => ServerNative.bison_set_bool(handle, key, b ? 1 : 0),
                int i => ServerNative.bison_set_int(handle, key, i),
                float f => ServerNative.bison_set_float(handle, key, f),
                double d => ServerNative.bison_set_float(handle, key, (float)d),
                string s => ServerNative.bison_set_string(handle, key, s),
                _ => throw new ArgumentException($"Unsupported params value type for '{name}': {value.GetType()}"),
            };
            if (rc != 0)
            {
                ServerNative.bison_release(handle);
                throw new InvalidOperationException($"failed to set params field '{name}' (bison_error {rc})");
            }
        }

        return new ServerParamsScope(handle);
    }
}

/// <summary>
/// RAII wrapper around a <c>wish_server_handle</c>. Construct via
/// <see cref="Tcp"/>, <see cref="Pipe"/>, <see cref="Tls"/>, or
/// <see cref="Term"/>, then <see cref="Start"/> to build the requested
/// renderer and begin accepting client connections.
/// </summary>
public sealed class Server : IDisposable
{
    private nint _handle;
    private bool _started;

    private Server(nint handle) => _handle = handle;

    public static Server Tcp(string host, ushort port)
    {
        var h = ServerNative.wish_server_tcp_create(host, port);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_server_tcp_create failed");
        }
        return new Server(h);
    }

    public static Server Pipe(string path)
    {
        var h = ServerNative.wish_server_pipe_create(path);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_server_pipe_create failed");
        }
        return new Server(h);
    }

    /// <summary>
    /// Creates a TLS-secured TCP server (not yet listening). TLS material
    /// (<c>cert_file</c>/<c>cert_pem</c>, <c>key_file</c>/<c>key_pem</c>,
    /// <c>key_password</c>, and optionally <c>client_auth</c>/
    /// <c>ca_file</c>/<c>ca_pem</c> for mutual TLS) is supplied via
    /// <see cref="Start"/>'s <c>parameters</c>.
    /// </summary>
    public static Server Tls(string host, ushort port)
    {
        var h = ServerNative.wish_server_tls_create(host, port);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_server_tls_create failed");
        }
        return new Server(h);
    }

    /// <summary>
    /// Creates a terminal (OSC-99 framed) server by spawning a child
    /// process attached to a new pseudo-terminal. The spawned child is
    /// expected to be a wish client process using <c>Client.Term()</c> (or
    /// an equivalent term-transport client) over its own inherited stdio.
    /// <see cref="ShouldQuit"/> also returns <c>true</c> once the spawned
    /// child exits, in addition to any renderer close signal.
    /// </summary>
    /// <param name="cmd">Command to exec in the child. Empty (default)
    /// spawns the operator's <c>$SHELL</c>/<c>cmd.exe</c>.</param>
    public static Server Term(string cmd = "")
    {
        var h = ServerNative.wish_server_term_create(cmd.Length == 0 ? null : cmd);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("wish_server_term_create failed");
        }
        return new Server(h);
    }

    /// <summary>Frees the server (stopping it first if still running).</summary>
    public void Destroy()
    {
        if (_handle != nint.Zero)
        {
            ServerNative.wish_server_destroy(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Destroy();
        GC.SuppressFinalize(this);
    }

    ~Server() => Destroy();

    public string LastError()
    {
        var ptr = ServerNative.wish_server_last_error(_handle);
        return ptr == nint.Zero ? "" : Marshal.PtrToStringUTF8(ptr) ?? "";
    }

    /// <summary>Deprecated: prefer <see cref="SetLogLevel"/>. <c>true</c> maps to <c>"trace"</c>, <c>false</c> to <c>"none"</c>. Must be called before <see cref="Start"/>.</summary>
    public void SetVerbose(bool verbose = true)
    {
        WishServerException.Check(ServerNative.wish_server_set_verbose(_handle, verbose ? 1 : 0), "set_verbose", LastError);
    }

    /// <summary>
    /// Sets log verbosity: one of <c>"none"</c>, <c>"fatal"</c>, <c>"error"</c>, <c>"warning"</c>, <c>"info"</c>, <c>"trace"</c>
    /// (default <c>"none"</c>). RMI trace lines appear at <c>"info"</c> and above, decoded payloads at <c>"trace"</c>.
    /// Must be called before <see cref="Start"/>.
    /// </summary>
    public void SetLogLevel(string level)
    {
        WishServerException.Check(ServerNative.wish_server_set_log_level(_handle, level), "set_log_level", LastError);
    }

    /// <summary>
    /// Builds <paramref name="renderer"/> and begins accepting client
    /// connections.
    /// </summary>
    /// <param name="renderer">
    /// <c>"sdl3"</c> (a real window), <c>"web"</c> (pass <c>web_bind</c>/
    /// <c>web_port</c>; open the printed URL in a browser), or
    /// <c>"console"</c> (a lightweight text dump of the widget tree to
    /// stdout -- no display needed, meant for tests/CI).
    /// </param>
    /// <param name="parameters">
    /// Renderer-specific fields, all optional: <c>title</c>, <c>width</c>,
    /// <c>height</c>, <c>font_size</c> for <c>"sdl3"</c>/<c>"web"</c>;
    /// <c>web_bind</c>, <c>web_port</c> for <c>"web"</c> only. Matches the
    /// <c>wish server</c> CLI's own flags/defaults. Also forwarded
    /// unchanged to the transport's own listen params -- e.g.
    /// <c>cert_file</c>/<c>key_file</c>/etc. for a server created with
    /// <see cref="Tls"/>; ignored by every other transport.
    /// </param>
    public void Start(string renderer, IReadOnlyDictionary<string, object>? parameters = null)
    {
        using var scope = ServerParamsScope.From(parameters);
        WishServerException.Check(
            ServerNative.wish_server_start(_handle, renderer, scope.Handle), $"start({renderer})", LastError);
        _started = true;
    }

    /// <summary>Stops the accept loop, render loop, and joins all threads.</summary>
    public void Stop()
    {
        if (!_started)
        {
            return;
        }
        WishServerException.Check(ServerNative.wish_server_stop(_handle), "stop", LastError);
        _started = false;
    }

    /// <summary>
    /// Returns <c>true</c> once the renderer signals it should close (e.g.
    /// the SDL3 window was closed), or -- for a <see cref="Term"/> server --
    /// once the spawned child process has exited. The web/console
    /// renderers never set this on their own; call <see cref="Stop"/>
    /// explicitly for those.
    /// </summary>
    public bool ShouldQuit() => ServerNative.wish_server_should_quit(_handle) != 0;
}
