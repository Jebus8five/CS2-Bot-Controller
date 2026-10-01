using BotControllerPocHarness;
using Xunit;

namespace BotControllerPocHarness.Tests;

public class AimArgParsingTests
{
    private static bool Parse(out AimRequest req, params string?[] args) =>
        AimSafety.TryParseSetAimArgs(args, out req, out _);

    [Fact]
    public void ParsesValidArgs()
    {
        Assert.True(Parse(out var req, "0", "-10.5", "45"));
        Assert.Equal(new AimRequest(0, -10.5f, 45f), req);
    }

    [Theory]
    [InlineData(-89f)]
    [InlineData(89f)]
    [InlineData(0f)]
    public void AcceptsPitchAtOrWithinBounds(float pitch) =>
        Assert.True(Parse(out _, "0", pitch.ToString(System.Globalization.CultureInfo.InvariantCulture), "0"));

    [Theory]
    [InlineData(89.0001f)]
    [InlineData(-89.0001f)]
    [InlineData(90f)]
    [InlineData(500f)]
    [InlineData(-500f)]
    public void RejectsPitchOutsideBounds(float pitch) =>
        Assert.False(Parse(out _, "0", pitch.ToString(System.Globalization.CultureInfo.InvariantCulture), "0"));

    [Theory]
    [InlineData(-540f)]
    [InlineData(-180f)]
    [InlineData(0f)]
    [InlineData(180f)]
    [InlineData(359.9f)]
    [InlineData(1000f)]
    public void YawIsUnconstrained(float yaw) =>
        Assert.True(Parse(out _, "0", "0", yaw.ToString(System.Globalization.CultureInfo.InvariantCulture)));

    [Theory]
    [InlineData("x", "0", "0")]
    [InlineData("-1", "0", "0")]
    [InlineData("1.5", "0", "0")]
    [InlineData("0", "NaN", "0")]
    [InlineData("0", "Infinity", "0")]
    [InlineData("0", "0", "NaN")]
    [InlineData("0", "0", "Infinity")]
    [InlineData("0", null, "0")]
    [InlineData("0", "0", null)]
    [InlineData("0", "0", "")]
    public void RejectsBadArgs(string? slot, string? pitch, string? yaw) =>
        Assert.False(Parse(out _, slot, pitch, yaw));

    [Fact]
    public void RejectsTooFewArgs() =>
        Assert.False(Parse(out _, "0", "1"));

    [Fact]
    public void UsesInvariantCultureDecimalPoint()
    {
        var saved = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("sv-SE");
            Assert.True(Parse(out var req, "0", "1.5", "0"));
            Assert.Equal(1.5f, req.Pitch);
            Assert.False(Parse(out _, "0", "1,5", "0"));
        }
        finally { System.Globalization.CultureInfo.CurrentCulture = saved; }
    }
}
