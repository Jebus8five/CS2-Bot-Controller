using BotControllerPocHarness;
using Xunit;

namespace BotControllerPocHarness.Tests;

public class FireArgParsingTests
{
    private static bool Parse(out FireTestRequest req, params string?[] args) =>
        FireSafety.TryParseFireTestArgs(args, out req, out _);

    [Fact]
    public void AllOptionalArgsOmittedUsesDefaults()
    {
        Assert.True(Parse(out var req, "0"));
        Assert.Equal(new FireTestRequest(0, FireSafety.DefaultPressCount, FireSafety.DefaultPressDurationMs,
            FireSafety.DefaultGapMs, FireLockMode.None, FireSafety.DefaultWeaponDefIndex), req);
    }

    [Fact]
    public void ParsesAllExplicitArgs()
    {
        Assert.True(Parse(out var req, "3", "5", "10", "150", "all", "7"));
        Assert.Equal(new FireTestRequest(3, 5, 10, 150, FireLockMode.All, 7), req);
    }

    [Theory]
    [InlineData("none", FireLockMode.None)]
    [InlineData("NONE", FireLockMode.None)]
    [InlineData("aim", FireLockMode.Aim)]
    [InlineData("AIM", FireLockMode.Aim)]
    [InlineData("all", FireLockMode.All)]
    [InlineData("ALL", FireLockMode.All)]
    [InlineData(null, FireLockMode.None)]
    [InlineData("", FireLockMode.None)]
    public void ParsesLockModeCaseInsensitively(string? lockMode, FireLockMode expected)
    {
        Assert.True(Parse(out var req, "0", null, null, null, lockMode));
        Assert.Equal(expected, req.LockMode);
    }

    [Fact]
    public void RejectsBadLockMode() =>
        Assert.False(Parse(out _, "0", null, null, null, "everything"));

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    [InlineData("0")]
    [InlineData("1.5")]
    public void RejectsBadPressCount(string pressCount) =>
        Assert.False(Parse(out _, "0", pressCount));

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    public void RejectsBadPressDurationMs(string duration) =>
        Assert.False(Parse(out _, "0", null, duration));

    [Fact]
    public void AcceptsZeroPressDurationMs()
    {
        // Verified from InputInjector.cpp's own PendingPress->PendingRelease
        // transition: durationMs=0 is a real, valid single-tick press, NOT
        // "no press" -- this harness must not reject it.
        Assert.True(Parse(out var req, "0", null, "0"));
        Assert.Equal(0, req.PressDurationMs);
    }

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    public void RejectsBadGapMs(string gap) =>
        Assert.False(Parse(out _, "0", null, null, gap));

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    public void RejectsBadWeaponDefIndex(string def) =>
        Assert.False(Parse(out _, "0", null, null, null, null, def));

    [Fact]
    public void RejectsBadSlot() =>
        Assert.False(Parse(out _, "-1"));

    [Fact]
    public void AttackButtonMaskIsBitZero() =>
        Assert.Equal(1UL, FireSafety.AttackButtonMask);
}
