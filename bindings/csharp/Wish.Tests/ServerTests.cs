// MIT License © 2025 Binary Dice Games
/**
 * @file ServerTests.cs
 * @brief Tests for Bdg.Wish.Server -- mirrors
 *        bindings/python/tests/test_server.py's lifecycle-only coverage.
 *
 * Build wish_server_dll first, then run from the repository root:
 *
 *   cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
 *   cmake --build build --target wish_server_dll
 *   dotnet test bindings/csharp/Wish.Tests
 */

using Bdg.Wish;
using Xunit;

namespace Bdg.Wish.Tests;

public class ServerLifecycleTests
{
    // Distinct ports per test so a slow OS socket teardown from one test
    // can't collide with the next.
    private static ushort NextPort() => (ushort)(17090 + System.Threading.Interlocked.Increment(ref _portOffset));
    private static int _portOffset;

    [Fact]
    public void TcpCreateAndDestroy()
    {
        var server = Server.Tcp("127.0.0.1", NextPort());
        server.Destroy();
        // Idempotent.
        server.Destroy();
    }

    [Fact]
    public void TlsCreateAndDestroy()
    {
        var server = Server.Tls("127.0.0.1", NextPort());
        server.Destroy();
        // Idempotent.
        server.Destroy();
    }

    [Fact]
    public void TermCreateAndDestroy()
    {
        var server = Server.Term("/bin/true");
        server.Destroy();
        // Idempotent.
        server.Destroy();
    }

    [Fact]
    public void StopBeforeStartIsNoop()
    {
        using var server = Server.Tcp("127.0.0.1", NextPort());
        server.Stop(); // must not throw even though Start() was never called
    }

    [Fact]
    public void ShouldQuitFalseBeforeStart()
    {
        using var server = Server.Tcp("127.0.0.1", NextPort());
        Assert.False(server.ShouldQuit());
    }

    [Fact]
    public void StartWithRendererParams()
    {
        using var server = Server.Tcp("127.0.0.1", NextPort());
        server.Start("console", new Dictionary<string, object> { ["title"] = "Custom Title", ["width"] = 800, ["height"] = 600 });
        server.Stop();
    }

    [Fact]
    public void BadRendererKindThrows()
    {
        using var server = Server.Tcp("127.0.0.1", NextPort());
        var ex = Assert.Throws<WishServerException>(() => server.Start("not-a-real-renderer"));
        Assert.Equal(WishServerErrorCode.BadRenderer, ex.Code);
    }
}
