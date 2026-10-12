// MIT License © 2026 Binary Dice Games
/**
 * @file DesktopTests.cs
 * @brief Tests for Client.Desktop() / RunOnDesktop() / StopDesktop() --
 *        mirrors bindings/python/tests/test_desktop.py's server-less cases.
 *
 * WISH_DESKTOP_ENDPOINT is pointed at a fresh, unused name so the loop
 * keeps retrying until stopped. (.NET on Unix may not pass the variable on
 * to native code; the tests hold either way, since StopDesktop() also ends
 * a session on a real desktop.)
 *
 *   cmake -B build
 *   cmake --build build --target wish_client_dll
 *   dotnet test bindings/csharp/Wish.Tests
 */

using System;
using System.Threading.Tasks;
using Bdg.Wish;
using Xunit;

namespace Bdg.Wish.Tests;

public class DesktopTests {
  private static void UseUnusedEndpoint() {
    var name = "wish_cs_desktop_test_" + Guid.NewGuid().ToString("N");
    Environment.SetEnvironmentVariable(
        "WISH_DESKTOP_ENDPOINT", OperatingSystem.IsWindows() ? @"\\.\pipe\" + name : "@" + name);
  }

  [Fact]
  public void DesktopCreateAndDestroy() {
    UseUnusedEndpoint();
    var client = Client.Desktop();
    client.Destroy();
    // Idempotent.
    client.Destroy();
  }

  [Fact]
  public void StopBeforeRunReturnsAtOnce() {
    UseUnusedEndpoint();
    using var client = Client.Desktop();
    client.StopDesktop();
    var ran = false;
    client.RunOnDesktop(
        _ => ran = true);
    Assert.False(ran);
  }

  [Fact]
  public void StopWhileWaitingForADesktopReturns() {
    UseUnusedEndpoint();
    using var client = Client.Desktop();
    var loop = Task.Run(() => client.RunOnDesktop(c => c.Wait(), null, TimeSpan.FromMilliseconds(20)));
    client.StopDesktop();
    Assert.True(loop.Wait(TimeSpan.FromSeconds(5)));
  }
}
