// MIT License © 2025 Binary Dice Games
/**
 * @file WishInterop.cs
 * @brief Bridges raw handles returned by `wish_*` calls into `Bdg.Bison.Rmi`
 *        wrapper types.
 *
 * `wish_client_c.h` returns plain `rmi_proxy_handle` / `rmi_future_handle`
 * values -- the exact same C types `Bdg.Bison.Rmi` already wraps -- so these
 * helpers just construct the wrapper around a handle obtained from a
 * `wish_*` call, using the internal constructors `Bdg.Bison`'s
 * `InternalsVisibleTo("Bdg.Wish")` grants this assembly access to (mirrors
 * `wish/client.py` constructing `bison.rmi.Proxy(h)` / `Future(h)` directly).
 */

using Bdg.Bison.Rmi;

namespace Bdg.Wish;

internal static class WishInterop
{
    public static Proxy WrapProxy(nint handle) => new(handle);

    public static Future WrapFuture(nint handle) => new(handle);

    public static void FreeString(nint ptr) => Bdg.Bison.Native.bison_free_string(ptr);
}
