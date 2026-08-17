// MIT License © 2025 Binary Dice Games
/**
 * @file Native.cs
 * @brief Source-generated P/Invoke declarations for `wish_client_c.h`, plus
 *        the library-resolution logic that locates `libwish_client.{so,
 *        dylib,dll}`.
 *
 * `libwish_client` embeds `bison_c.h`/`rmi_c.h` alongside `wish_client_c.h`
 * in one shared object (see the `wish_client_dll` CMake target), so this
 * class's resolver -- once it has found that one file -- also points
 * `Bdg.Bison.Native`'s own resolver at it via the `BISON_LIB` environment
 * variable, the same way `wish/_native.py`'s `get_lib()` does with
 * `os.environ.setdefault("BISON_LIB", path)`. That is why `Bdg.Bison.Dynamic`
 * / `Bdg.Bison.Rmi.Proxy` / `Bdg.Bison.Rmi.Future` work unmodified against
 * wish's `bison_handle` / `rmi_proxy_handle` / `rmi_future_handle` values --
 * they resolve against the exact same loaded shared object.
 */

using System.Runtime.InteropServices;

namespace Bdg.Wish;

/// <summary>Mirrors <c>wish_error</c> return codes.</summary>
public enum WishErrorCode
{
    Ok = 0,
    Null = -1,
    NotFound = -2,
    Transport = -3,
    Exception = -4,
    Ambiguous = -5,
}

/// <summary><c>wish_session_fn</c>: void(*)(wish_client_handle client, void* userdata).</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void NativeSessionFn(nint client, nint userdata);

internal static partial class Native
{
    private const string LibName = "wish_client";

    /// <summary>
    /// Locates <c>libwish_client</c> the same way <c>wish/_native.py</c>'s
    /// <c>_find_library()</c> does: an explicit <c>WISH_LIB</c> path first,
    /// then a handful of conventional build-output locations relative to the
    /// repo root, then the OS's normal library search. On success, also
    /// points <see cref="Bdg.Bison.Key"/>'s own library resolution at the
    /// same file via <c>BISON_LIB</c> (see the class doc comment) unless the
    /// caller already set one explicitly.
    ///
    /// Registered with the runtime by <see cref="NativeResolvers"/>, not by
    /// this class directly -- <c>NativeLibrary.SetDllImportResolver</c> is
    /// one-per-assembly, and <see cref="ServerNative"/> lives in this same
    /// assembly with its own, independent library to resolve.
    /// </summary>
    internal static nint ResolveLibrary(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != LibName)
        {
            return nint.Zero;
        }

        var envPath = Environment.GetEnvironmentVariable("WISH_LIB");
        if (!string.IsNullOrEmpty(envPath) && NativeLibrary.TryLoad(envPath, out var envHandle))
        {
            AdoptAsBisonLib(envPath);
            return envHandle;
        }

        var repoRoot = FindRepoRoot(AppContext.BaseDirectory);
        if (repoRoot is not null)
        {
            string[] candidates =
            [
                Path.Combine(repoRoot, "build", "libwish_client.so"),
                Path.Combine(repoRoot, "build", "libwish_client.dylib"),
                Path.Combine(repoRoot, "build", "Release", "wish_client.dll"),
                Path.Combine(repoRoot, "build", "Debug", "wish_client.dll"),
                Path.Combine(repoRoot, "build", "wish_client.dll"),
            ];
            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var handle))
                {
                    AdoptAsBisonLib(candidate);
                    return handle;
                }
            }
        }

        return nint.Zero;
    }

    private static void AdoptAsBisonLib(string path)
    {
        if (Environment.GetEnvironmentVariable("BISON_LIB") is null)
        {
            Environment.SetEnvironmentVariable("BISON_LIB", path);
        }
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

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial uint wish_key(string name);

    // ── Client lifecycle ──────────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_client_tcp_create(string host, ushort port);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_client_tls_create(string host, ushort port);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_client_stream_create(string path);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_client_pipe_create(string path);

    [LibraryImport(LibName)]
    public static partial nint wish_client_term_create();

    [LibraryImport(LibName)]
    public static partial void wish_client_destroy(nint client);

    [LibraryImport(LibName)]
    public static partial int wish_client_run(nint client, nint sessionFn, nint userdata);

    [LibraryImport(LibName)]
    public static partial int wish_client_run_with_params(nint client, nint sessionFn, nint userdata, nint connectParams);

    [LibraryImport(LibName)]
    public static partial void wish_client_wait(nint client);

    [LibraryImport(LibName)]
    public static partial void wish_client_quit(nint client);

    [LibraryImport(LibName)]
    public static partial nint wish_last_error(nint client);

    // ── Style ─────────────────────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_set_style_preset(nint client, string preset);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_set_style_preset_async(nint client, string preset, out nint outFuture);

    // ── Template management ───────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_register_template(nint client, string name, string descriptor);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_register_template_async(nint client, string name, string descriptor, out nint outFuture);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_instantiate_template(nint client, string name, string prefix);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_instantiate_template_async(nint client, string name, string prefix, out nint outFuture);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial nint wish_proxy_get(nint client, string dotPath);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_release(nint client, string prefix);

    // ── Object instantiation ──────────────────────────────────────────────────

    [LibraryImport(LibName)]
    public static partial nint wish_instantiate(nint client, uint ns, uint klass, nint parameters);

    // ── Embedded apps ─────────────────────────────────────────────────────────

    [LibraryImport(LibName)]
    public static partial int wish_list_apps(out nint outJson);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_run_app(nint client, string appName, string[] args, nuint nargs);

    // ── File transfer ──────────────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_upload_file(nint client, string name, byte[] data, nuint dataLen);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_download_file(nint client, string name, out nint outData, out nuint outLen);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_upload_file_from_path(nint client, string name, string localPath);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_download_file_to_path(nint client, string name, string localPath);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_upload_package_from_path(nint client, string destPath, string localZipPath);

    // ── Logging ───────────────────────────────────────────────────────────────

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_log(nint client, string level, string msg);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_log_debug(nint client, string msg);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_log_info(nint client, string msg);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_log_warn(nint client, string msg);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    public static partial int wish_log_error(nint client, string msg);
}
