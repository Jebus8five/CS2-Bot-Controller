using BotControllerPocHarness;
using Xunit;

namespace BotControllerPocHarness.Tests;

public class IsolatedModuleDirectoryTests
{
    private const string Canonical = TeleportSafety.IsolatedHarnessModuleDirectory;

    [Theory]
    [InlineData(Canonical)]
    [InlineData(Canonical + @"\")]
    [InlineData(@"c:\cs2isolatedbctest\server\game\csgo\addons\counterstrikesharp\plugins\botcontrollerpocharness")]
    public void AcceptsOnlyTheCanonicalDirectory(string dir) =>
        Assert.True(TeleportSafety.IsCanonicalIsolatedModuleDirectory(dir));

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(" ")]
    // Backups and other installs that the old substring check accepted.
    [InlineData(@"C:\CS2IsolatedBCTest-harness-backup-20260925-073029\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"C:\CS2IsolatedBCTest-v063-backup-20260924-223128\addons\BotController")]
    [InlineData(@"D:\CS2IsolatedBCTest\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"C:\Prod\CS2IsolatedBCTest\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"C:\CS2BotControllerV07Test\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"C:\CS2IsolatedBCTest")]
    // Non-canonical spellings of the right place.
    [InlineData(Canonical + @"\\")]
    [InlineData(Canonical + @"\.")]
    [InlineData(Canonical + @"\sub")]
    [InlineData(@"C:\CS2IsolatedBCTest\server\..\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"C:/CS2IsolatedBCTest/server/game/csgo/addons/counterstrikesharp/plugins/BotControllerPocHarness")]
    [InlineData(@"\\?\C:\CS2IsolatedBCTest\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(@"\\server\C$\CS2IsolatedBCTest\server\game\csgo\addons\counterstrikesharp\plugins\BotControllerPocHarness")]
    [InlineData(" " + Canonical)]
    [InlineData(Canonical + " ")]
    public void RejectsEverythingElse(string? dir) =>
        Assert.False(TeleportSafety.IsCanonicalIsolatedModuleDirectory(dir));
}

public class TeleportBlockReasonTests
{
    [Fact]
    public void AllowsWhenNothingIsActive() =>
        Assert.Null(TeleportSafety.TeleportBlockReason(false, false, false, false, false, false));

    [Theory]
    [InlineData(0)] // gate2Active
    [InlineData(1)] // gate2PostCancelActive
    [InlineData(2)] // gate3Active
    [InlineData(3)] // gate2cWaitingForRest
    [InlineData(4)] // gate2cActive
    [InlineData(5)] // gate2cPostCancelActive
    public void BlocksWhenAnySinglePhaseIsActive(int which)
    {
        var f = new bool[6];
        f[which] = true;
        Assert.NotNull(TeleportSafety.TeleportBlockReason(f[0], f[1], f[2], f[3], f[4], f[5]));
    }

    [Fact]
    public void BlocksWhenEverythingIsActive() =>
        Assert.NotNull(TeleportSafety.TeleportBlockReason(true, true, true, true, true, true));
}

public class TeleportArgParsingTests
{
    private static bool Parse(out TeleportRequest req, params string?[] args) =>
        TeleportSafety.TryParseTeleportArgs(args, out req, out _);

    [Fact]
    public void ParsesRequiredArgsWithDefaultRoll()
    {
        Assert.True(Parse(out var req, "3", "100.5", "-200", "64", "-10", "90"));
        Assert.Equal(new TeleportRequest(3, 100.5f, -200f, 64f, -10f, 90f, 0f), req);
    }

    [Fact]
    public void ParsesExplicitRoll()
    {
        Assert.True(Parse(out var req, "0", "1", "2", "3", "4", "5", "6.25"));
        Assert.Equal(6.25f, req.Roll);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void MissingRollDefaultsToZero(string? roll)
    {
        Assert.True(Parse(out var req, "0", "1", "2", "3", "4", "5", roll));
        Assert.Equal(0f, req.Roll);
    }

    [Fact]
    public void UsesInvariantCultureDecimalPoint()
    {
        var saved = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("sv-SE");
            Assert.True(Parse(out var req, "0", "1.5", "0", "0", "0", "0"));
            Assert.Equal(1.5f, req.X);
            Assert.False(Parse(out _, "0", "1,5", "0", "0", "0", "0"));
        }
        finally { System.Globalization.CultureInfo.CurrentCulture = saved; }
    }

    [Theory]
    [InlineData("x", "1", "2", "3", "4", "5")]
    [InlineData("-1", "1", "2", "3", "4", "5")]
    [InlineData("1.5", "1", "2", "3", "4", "5")]
    [InlineData("0", "NaN", "2", "3", "4", "5")]
    [InlineData("0", "1", "Infinity", "3", "4", "5")]
    [InlineData("0", "1", "2", "-Infinity", "4", "5")]
    [InlineData("0", "1", "2", "3", "1e39", "5")]
    [InlineData("0", "1", "2", "3", "4", "abc")]
    [InlineData("0", "1", "2", "3", "4", null)]
    [InlineData("0", "1", "2", "3", "4", "")]
    public void RejectsBadRequiredArgs(string? slot, string? x, string? y, string? z, string? pitch, string? yaw) =>
        Assert.False(Parse(out _, slot, x, y, z, pitch, yaw));

    [Theory]
    [InlineData("NaN")]
    [InlineData("Infinity")]
    [InlineData("abc")]
    public void RejectsBadRoll(string roll) =>
        Assert.False(Parse(out _, "0", "1", "2", "3", "4", "5", roll));

    [Fact]
    public void RejectsTooFewArgs() =>
        Assert.False(Parse(out _, "0", "1", "2"));
}

public class AngleAndExpiryTests
{
    [Theory]
    [InlineData(90f, 90f, 0f)]
    [InlineData(179f, -179f, -2f)]
    [InlineData(-179f, 179f, 2f)]
    [InlineData(360f, 0f, 0f)]
    [InlineData(10f, 350f, 20f)]
    [InlineData(0f, 180f, -180f)]
    [InlineData(-90f, 450f, 180f - 360f)]
    public void WrapsDeltaIntoHalfOpenRange(float observed, float requested, float expected)
    {
        var d = TeleportSafety.WrapAngleDelta(observed, requested);
        Assert.InRange(d, -180f, 179.9999f);
        Assert.Equal(expected, d, 3);
    }

    [Fact]
    public void ExpiryIsInclusiveOfDeadline()
    {
        Assert.False(TeleportSafety.IsVerificationExpired(nowMs: 999, expiresAtMs: 1000));
        Assert.True(TeleportSafety.IsVerificationExpired(nowMs: 1000, expiresAtMs: 1000));
        Assert.True(TeleportSafety.IsVerificationExpired(nowMs: 5000, expiresAtMs: 1000));
    }

    [Fact]
    public void ExpiryComfortablyExceedsTheTickDelay()
    {
        // Even at a very low 16 tick/s the tick delay must elapse well before expiry.
        Assert.True(TeleportSafety.VerifyDelayTicks * (1000.0 / 16) < TeleportSafety.VerifyExpiryMs / 4.0);
    }
}
