using System.Globalization;
using System.Text.RegularExpressions;

namespace Reborn.Update;

/// <summary>Release numbers 1.0.N, where N is the build. Same rules as tools/version.py:
/// anything that is not exactly 1.0.N (N from 1, no leading zero) - a git SHA, empty text, 1.0.03 -
/// is build 0, a developer build.</summary>
public static partial class Versions
{
    [GeneratedRegex(@"^1\.0\.([1-9][0-9]*)\z", RegexOptions.CultureInvariant)]
    private static partial Regex Release();

    /// <summary>'1.0.7' -> 7. Anything else -> 0.</summary>
    public static int Parse(string text)
    {
        Match m = Release().Match((text ?? "").Trim());
        return m.Success && int.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out int build)
            ? build
            : 0;
    }

    /// <summary>True when <paramref name="build"/> is a later release than <paramref name="current"/>.
    /// Any release is newer than a developer build (0).</summary>
    public static bool Newer(int build, int current) => build > current;
}
