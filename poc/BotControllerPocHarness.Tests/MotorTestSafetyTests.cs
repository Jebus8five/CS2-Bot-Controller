using BotControllerPocHarness;
using Xunit;

namespace BotControllerPocHarness.Tests;

public class JumpArgParsingTests
{
    private static bool Parse(out JumpTestRequest req, params string?[] args) =>
        MotorTestSafety.TryParseJumpTestArgs(args, out req, out _);

    [Fact]
    public void DefaultsWhenOmitted()
    {
        Assert.True(Parse(out var req, "0"));
        Assert.Equal(new JumpTestRequest(0, MotorTestSafety.DefaultJumpCount, MotorTestSafety.DefaultJumpPressDurationMs, FireLockMode.None), req);
    }

    [Fact]
    public void ParsesExplicitArgs()
    {
        Assert.True(Parse(out var req, "2", "5", "40", "all"));
        Assert.Equal(new JumpTestRequest(2, 5, 40, FireLockMode.All), req);
    }

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    [InlineData("0")]
    public void RejectsBadJumpCount(string jumpCount) => Assert.False(Parse(out _, "0", jumpCount));

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    public void RejectsBadPressDurationMs(string duration) => Assert.False(Parse(out _, "0", null, duration));

    [Fact]
    public void AcceptsZeroPressDurationMs() => Assert.True(Parse(out _, "0", null, "0"));

    [Fact]
    public void RejectsBadLockMode() => Assert.False(Parse(out _, "0", null, null, "everything"));

    [Fact]
    public void JumpButtonMaskIsBitOne() => Assert.Equal(2UL, MotorTestSafety.JumpButtonMask);
}

public class CrouchArgParsingTests
{
    private static bool Parse(out CrouchTestRequest req, params string?[] args) =>
        MotorTestSafety.TryParseCrouchTestArgs(args, out req, out _);

    [Fact]
    public void DefaultsWhenOmitted()
    {
        Assert.True(Parse(out var req, "0"));
        Assert.Equal(new CrouchTestRequest(0, MotorTestSafety.DefaultHoldCount, MotorTestSafety.DefaultHoldMs, FireLockMode.None), req);
    }

    [Fact]
    public void ParsesExplicitArgs()
    {
        Assert.True(Parse(out var req, "1", "3", "1200", "aim"));
        Assert.Equal(new CrouchTestRequest(1, 3, 1200, FireLockMode.Aim), req);
    }

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    [InlineData("0")]
    public void RejectsBadHoldCount(string holdCount) => Assert.False(Parse(out _, "0", holdCount));

    [Theory]
    [InlineData("x")]
    [InlineData("-1")]
    [InlineData("0")]
    public void RejectsBadHoldMs(string holdMs) => Assert.False(Parse(out _, "0", null, holdMs));

    [Fact]
    public void RejectsBadLockMode() => Assert.False(Parse(out _, "0", null, null, "everything"));

    [Fact]
    public void DuckButtonMaskIsBitTwo() => Assert.Equal(4UL, MotorTestSafety.DuckButtonMask);
}
