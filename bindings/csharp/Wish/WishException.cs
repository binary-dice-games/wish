// MIT License © 2025 Binary Dice Games
/**
 * @file WishException.cs
 * @brief Exception type raised when a `wish_*` C API call returns a
 *        non-zero error code.
 */

namespace Bdg.Wish;

/// <summary>Raised when a <c>wish_*</c> C API call returns a non-zero error code.</summary>
public sealed class WishException : Exception
{
    private static readonly Dictionary<WishErrorCode, string> Messages = new()
    {
        [WishErrorCode.Null] = "Null pointer argument",
        [WishErrorCode.NotFound] = "Named proxy or resource not found",
        [WishErrorCode.Transport] = "Transport connection failed",
        [WishErrorCode.Exception] = "Internal C++ exception",
        [WishErrorCode.Ambiguous] = "App name matches more than one registered app; use the fully-qualified name (see LastError())",
    };

    public WishErrorCode Code { get; }

    public WishException(WishErrorCode code, string context = "")
        : base(Format(code, context))
    {
        Code = code;
    }

    private static string Format(WishErrorCode code, string context)
    {
        var msg = Messages.TryGetValue(code, out var m) ? m : $"Unknown error {(int)code}";
        return context.Length > 0 ? $"{context}: {msg}" : msg;
    }

    internal static void Check(int rc, string context = "")
    {
        if (rc != (int)WishErrorCode.Ok)
        {
            throw new WishException((WishErrorCode)rc, context);
        }
    }
}
