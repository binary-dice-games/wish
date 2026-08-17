// MIT License © 2025 Binary Dice Games
/**
 * @file NativeResolvers.cs
 * @brief Registers this assembly's single `DllImportResolver`, dispatching
 *        between `Native` (`libwish_client`) and `ServerNative`
 *        (`libwish_server`) by library name.
 *
 * `NativeLibrary.SetDllImportResolver` accepts at most one resolver per
 * assembly -- `Native` and `ServerNative` both live in `Bdg.Wish`, so
 * neither can call it from its own static constructor (the second call
 * throws `InvalidOperationException: A resolver is already set for the
 * assembly`). This module initializer registers one combined resolver
 * instead, before any other code in the assembly runs.
 */

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Bdg.Wish;

internal static class NativeResolvers
{
    [ModuleInitializer]
    internal static void Init()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeResolvers).Assembly, Resolve);
    }

    private static nint Resolve(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        var fromClient = Native.ResolveLibrary(libraryName, assembly, searchPath);
        if (fromClient != nint.Zero)
        {
            return fromClient;
        }
        return ServerNative.ResolveLibrary(libraryName, assembly, searchPath);
    }
}
