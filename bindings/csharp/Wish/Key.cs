// MIT License © 2025 Binary Dice Games
/**
 * @file Key.cs
 * @brief Name hashing for the wish C# binding.
 */

namespace Bdg.Wish;

/// <summary>
/// Computes the FNV-1a hash of a field/method/template name -- identical to
/// <see cref="Bdg.Bison.Key.Of"/> and the C++ <c>"name"_key</c> literal.
///
/// <c>wish_client_c.h</c> exposes its own <c>wish_key()</c> entry point
/// purely so C callers linking only against <c>wish_client_c.h</c> don't
/// need to also pull in <c>bison_c.h</c> (see that header's doc comment) --
/// but it is documented to produce the exact same value as <c>bison_key()</c>
/// for the same input. Since this binding already depends on
/// <c>Bdg.Bison</c> for <see cref="Bdg.Bison.Dynamic"/> /
/// <see cref="Bdg.Bison.Rmi.Proxy"/>, that modularity benefit doesn't apply
/// here, so this simply delegates to <see cref="Bdg.Bison.Key.Of"/> and
/// reuses its bounded memoization cache rather than round-tripping through
/// <c>wish_key()</c> a second time for a value that is already cached.
/// </summary>
public static class Key
{
    public static uint Of(string name) => Bdg.Bison.Key.Of(name);
}
