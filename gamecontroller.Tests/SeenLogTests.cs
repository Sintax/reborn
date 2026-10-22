using gamecontroller.Services;
using System.Text.Json;

namespace gamecontroller.Tests;

public class SeenLogTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "gcseen-" + Guid.NewGuid().ToString("N"));
    string FilePath => Path.Combine(_dir, "reborn-users-seen.json");
    public SeenLogTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { } }

    static readonly DateTime T0 = new(2026, 10, 22, 18, 0, 0, DateTimeKind.Utc);

    // The names and times in the saved file, or null when there is no file.
    Dictionary<string, string>? Saved()
    {
        if (!File.Exists(FilePath)) return null;
        using var doc = JsonDocument.Parse(File.ReadAllText(FilePath));
        return doc.RootElement.GetProperty("Seen").EnumerateObject().ToDictionary(p => p.Name, p => p.Value.GetString()!);
    }

    [Fact]
    public void Seen_names_are_case_insensitive()
    {
        DateTime now = T0;
        var seen = new SeenLog(FilePath, () => now);

        seen.Saw(new[] { "Sintax" });
        now = T0.AddMinutes(5);
        seen.Saw(new[] { "SINTAX" });

        Assert.Equal(T0.AddMinutes(5), seen.LastSeen("sintax"));
        Assert.Equal(T0.AddMinutes(5), seen.LastSeen("Sintax"));
        Assert.Null(seen.LastSeen("Nobody"));
        Assert.Single(Saved()!);
    }

    [Fact]
    public void Seen_log_saves_at_most_once_a_minute()
    {
        DateTime now = T0;
        var seen = new SeenLog(FilePath, () => now);

        seen.Saw(new[] { "Sintax" });
        Assert.Equal(new Dictionary<string, string> { ["Sintax"] = "2026-10-22T18:00:00Z" }, Saved());

        now = T0.AddSeconds(30);
        seen.Saw(new[] { "Friend" });
        Assert.Equal(new[] { "Sintax" }, Saved()!.Keys);   // too soon: not written yet

        now = T0.AddSeconds(61);
        seen.Saw(new[] { "Sintax" });
        Assert.Equal(new Dictionary<string, string> { ["Sintax"] = "2026-10-22T18:01:01Z", ["Friend"] = "2026-10-22T18:00:30Z" }, Saved());

        now = T0.AddSeconds(70);
        seen.Saw(new[] { "Late" });
        seen.Dispose();   // shutdown writes what is left
        Assert.Equal("2026-10-22T18:01:10Z", Saved()!["Late"]);
        Assert.False(File.Exists(FilePath + ".tmp"));

        var reloaded = new SeenLog(FilePath, () => now);
        Assert.Equal(T0.AddSeconds(70), reloaded.LastSeen("late"));
        Assert.Equal(DateTimeKind.Utc, reloaded.LastSeen("Friend")!.Value.Kind);
    }

    [Theory]
    [InlineData("not json at all")]
    [InlineData("[1,2,3]")]
    [InlineData("{\"Seen\":[\"Sintax\"]}")]
    public void Seen_log_survives_a_bad_file(string content)
    {
        File.WriteAllText(FilePath, content);
        var log = new ListLogger<SeenLog>();

        var seen = new SeenLog(FilePath, () => T0, log);

        Assert.Null(seen.LastSeen("Sintax"));
        Assert.Single(log.Lines);
        seen.Saw(new[] { "Sintax" });
        Assert.Equal(T0, seen.LastSeen("Sintax"));
        Assert.Equal(new[] { "Sintax" }, Saved()!.Keys);
    }

    [Fact]
    public void A_missing_file_starts_empty_with_one_line()
    {
        var log = new ListLogger<SeenLog>();
        var seen = new SeenLog(FilePath, () => T0, log);
        Assert.Null(seen.LastSeen("Sintax"));
        Assert.Single(log.Lines);
    }

    [Fact]
    public void Empty_names_are_not_recorded()
    {
        var seen = new SeenLog(FilePath, () => T0);
        seen.Saw(new[] { "", "Sintax" });
        Assert.Null(seen.LastSeen(""));
        Assert.Equal(new[] { "Sintax" }, Saved()!.Keys);
    }
}
