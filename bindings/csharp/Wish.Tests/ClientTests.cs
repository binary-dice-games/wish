// MIT License © 2025 Binary Dice Games
/**
 * @file ClientTests.cs
 * @brief Tests for Bdg.Wish.Client -- mirrors
 *        bindings/python/tests/test_client.py.
 *
 * Exercises only the parts that don't require a live wish server (there is
 * no standalone/in-process mode on the client-only wish ABI): handle
 * construction/destruction, key hashing, and error-code plumbing.
 *
 * Build wish_client_dll first, then run from the repository root:
 *
 *   cmake -B build
 *   cmake --build build --target wish_client_dll
 *   dotnet test bindings/csharp/Wish.Tests
 */

using Bdg.Bison;
using Bdg.Wish;
using Xunit;

namespace Bdg.Wish.Tests;

public class KeyHashingTests
{
    [Fact]
    public void KeyMatchesBisonKey()
    {
        Assert.Equal(Bdg.Bison.Key.Of("clicked"), Bdg.Wish.Key.Of("clicked"));
    }

    [Fact]
    public void KeyDeterministic()
    {
        Assert.Equal(Bdg.Wish.Key.Of("hello"), Bdg.Wish.Key.Of("hello"));
        Assert.NotEqual(Bdg.Wish.Key.Of("hello"), Bdg.Wish.Key.Of("world"));
    }
}

public class ClientLifecycleTests
{
    [Fact]
    public void TcpCreateAndDestroy()
    {
        var client = Client.Tcp("127.0.0.1", 7070);
        client.Destroy();
        // Idempotent.
        client.Destroy();
    }

    [Fact]
    public void DestroyViaDisposeDoesNotThrow()
    {
        using var client = Client.Tcp("127.0.0.1", 7070);
    }

    [Fact]
    public void LastErrorEmptyBeforeAnyOperation()
    {
        using var client = Client.Tcp("127.0.0.1", 7070);
        Assert.Equal("", client.LastError());
    }
}

public class ErrorMappingTests
{
    [Fact]
    public void WishExceptionMessage()
    {
        var ex = new WishException(WishErrorCode.NotFound, "proxy_get('x')");
        Assert.Contains("Named proxy or resource not found", ex.Message);
        Assert.Equal(WishErrorCode.NotFound, ex.Code);
    }
}

/// <summary>
/// Exercises Client.Run(sessionFn, parameters) -&gt; wish_client_run_with_params
/// plumbing (see src/auth/DESIGN.md). No live server is spun up here (the
/// client-only wish ABI has no standalone mode) -- connecting to a closed
/// port still proves parameters are marshaled through the FFI boundary
/// without crashing; the full persistence round-trip through a live server
/// is covered by the C++ integration tests in tests/test_auth.cpp.
/// </summary>
public class RunWithConnectParamsTests
{
    // Port 1 is a reserved/privileged port essentially never listening.
    private static Client UnreachableClient() => Client.Tcp("127.0.0.1", 1);

    [Fact]
    public void RunWithDictParamsFailsCleanlyNotACrash()
    {
        using var client = UnreachableClient();
        var ex = Assert.Throws<WishException>(() => client.Run(_ => { }, new Dictionary<string, object?> { ["username"] = "alice" }));
        Assert.Equal(WishErrorCode.Exception, ex.Code);
    }

    [Fact]
    public void RunWithNoParamsStillWorks()
    {
        using var client = UnreachableClient();
        var ex = Assert.Throws<WishException>(() => client.Run(_ => { }));
        Assert.Equal(WishErrorCode.Exception, ex.Code);
    }

    [Fact]
    public void RunWithDynamicParamsFailsCleanlyNotACrash()
    {
        using var client = UnreachableClient();
        using var parameters = new Dynamic();
        parameters["username"] = "alice";
        var ex = Assert.Throws<WishException>(() => client.Run(_ => { }, parameters));
        Assert.Equal(WishErrorCode.Exception, ex.Code);
    }
}
