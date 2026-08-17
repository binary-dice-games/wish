// MIT License © 2025 Binary Dice Games
/**
 * @file WishServerException.cs
 * @brief Exception type raised when a `wish_server_*` C API call returns a
 *        non-zero error code.
 */

namespace Bdg.Wish;

/// <summary>Raised when a <c>wish_server_*</c> C API call returns a non-zero error code.</summary>
public sealed class WishServerException : Exception
{
    private static readonly Dictionary<WishServerErrorCode, string> Messages = new()
    {
        [WishServerErrorCode.Null] = "Null handle or pointer",
        [WishServerErrorCode.Transport] = "Transport listen failed",
        [WishServerErrorCode.Exception] = "Internal C++ exception",
        [WishServerErrorCode.BadRenderer] = "Unknown renderer_kind, or this library wasn't built with support for it",
    };

    public WishServerErrorCode Code { get; }

    public WishServerException(WishServerErrorCode code, string context = "", string detail = "")
        : base(Format(code, context, detail))
    {
        Code = code;
    }

    private static string Format(WishServerErrorCode code, string context, string detail)
    {
        var msg = Messages.TryGetValue(code, out var m) ? m : $"Unknown error {(int)code}";
        if (detail.Length > 0)
        {
            msg = $"{msg} ({detail})";
        }
        return context.Length > 0 ? $"{context}: {msg}" : msg;
    }

    internal static void Check(int rc, string context = "", Func<string>? lastError = null)
    {
        if (rc != (int)WishServerErrorCode.Ok)
        {
            throw new WishServerException((WishServerErrorCode)rc, context, lastError?.Invoke() ?? "");
        }
    }
}
