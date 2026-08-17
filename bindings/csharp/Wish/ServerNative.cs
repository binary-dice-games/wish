// MIT License © 2025 Binary Dice Games
/**
 * @file ServerNative.cs
 * @brief Source-generated P/Invoke declarations for `wish_server_c.h`, plus
 *        the library-resolution logic that locates
 *        `libwish_server.{so,dylib,dll}`.
 *
 * A *separate* shared library from `wish_client`/`libwish_client` (see
 * `wish_server_c.h`'s file doc comment): `libwish_server` hosts and renders
 * a real wish session (SDL3 window, the web/browser renderer, or a
 * lightweight text `console` dump) over the real `bdg::wish::server` C++
 * implementation -- not the generic bison RMI server primitives -- and
 * embeds its own copy of the `bison_c.h`/`rmi_c.h` C ABI, separate from
 * `libwish_client`'s copy. A `bison_handle` from one is only ever valid
 * against the exact library that created it.
 *
 * This class therefore does **not** redirect `Bdg.Bison.Native`'s own
 * library resolution (unlike `Native.cs`'s `AdoptAsBisonLib`): if a process
 * used both `Bdg.Wish.Client` (which redirects `Bdg.Bison.Native` onto
 * `wish_client`) and `Bdg.Wish.Server` in the same process, whichever
 * redirected first would silently win, leaving the other's `Dynamic`/
 * `Proxy` objects bound to the wrong library. Instead, `ServerParams`
 * (see `Server.cs`) calls this class's own embedded `bison_*` functions
 * directly -- see `bindings/python/wish/_server_native.py`'s module doc
 * comment for the identical hazard and fix in the Python binding.
 */

using System.Runtime.InteropServices;

namespace Bdg.Wish;

/// <summary>Mirrors <c>wish_server_error</c> return codes.</summary>
public enum WishServerErrorCode
{
    Ok = 0,
    Null = -1,
    Transport = -3,
    Exception = -4,
    BadRenderer = -6,
}

internal static partial class ServerNative
{
    private const string LibName = "wish_server";

    /// <summary>
    /// Locates <c>libwish_server</c> the same way <c>_server_native.py</c>'s
    /// <c>_find_library()</c> does: an explicit <c>WISH_SERVER_LIB</c> path
    /// first, then a handful of conventional build-output locations
    /// relative to the repo root, then the OS's normal library search.
    ///
    /// Registered with the runtime by <see cref="NativeResolvers"/>, not by
    /// this class directly -- see <see cref="Native.ResolveLibrary"/>'s doc
    /// comment for why.
    /// </summary>
    internal static nint ResolveLibrary(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != LibName)
        {
            return nint.Zero;
        }

        var envPath = Environment.GetEnvironmentVariable("WISH_SERVER_LIB");
        if (!string.IsNullOrEmpty(envPath) && NativeLibrary.TryLoad(envPath, out var envHandle))
        {
            return envHandle;
        }

        var repoRoot = FindRepoRoot(AppContext.BaseDirectory);
        if (repoRoot is not null)
        {
            string[] candidates =
            [
                Path.Combine(repoRoot, "build", "libwish_server.so"),
                Path.Combine(repoRoot, "build", "libwish_server.dylib"),
                Path.Combine(repoRoot, "build", "Release", "wish_server.dll"),
                Path.Combine(repoRoot, "build", "Debug", "wish_server.dll"),
                Path.Combine(repoRoot, "build", "wish_server.dll"),
            ];
            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var handle))
                {
                    return handle;
                }
            }
        }

        return nint.Zero;
    }

    private static string? FindRepoRoot(string start)
    {
        var dir = new DirectoryInfo(start);
        while (dir is not null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "bindings")) && File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return null;
    }

    // ── Server lifecycle ─────────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_server_tcp_create(string host, ushort port);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_server_pipe_create(string path);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_server_tls_create(string host, ushort port);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_server_term_create(string? cmd);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_server_start(nint server, string rendererKind, nint parameters);

    [LibraryImport(LibName)]
    public static partial int wish_server_stop(nint server);

    [LibraryImport(LibName)]
    public static partial int wish_server_should_quit(nint server);

    [LibraryImport(LibName)]
    public static partial int wish_server_set_verbose(nint server, int verbose);

    [LibraryImport(LibName)]
    public static partial void wish_server_destroy(nint server);

    [LibraryImport(LibName)]
    public static partial nint wish_server_last_error(nint server);

    // ── bison_* subset needed to build wish_server_start()'s `parameters` ───
    // (This library embeds the full bison C ABI -- see the class doc
    // comment for why these are called directly, not via Bdg.Bison.Native.)

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial uint bison_key(string name);

    [LibraryImport(LibName)]
    public static partial nint bison_create(uint klass);

    [LibraryImport(LibName)]
    public static partial void bison_release(nint handle);

    [LibraryImport(LibName)]
    public static partial int bison_set_int(nint handle, uint name, int value);

    [LibraryImport(LibName)]
    public static partial int bison_set_float(nint handle, uint name, float value);

    [LibraryImport(LibName)]
    public static partial int bison_set_bool(nint handle, uint name, int value);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int bison_set_string(nint handle, uint name, string value);
}
