using System.Text.Json;
using gamecontroller.Services;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace gamecontroller.Tests;

public class UserListTests : IDisposable
{
    const string DjKey = "ABCDEFGHJKMNPQRSTVWXYZ23";
    const string MikeKey = "ZYXWVTSRQPNMKJHGFEDCBA32";

    readonly string _dir = Path.Combine(Path.GetTempPath(), "userlist-" + Guid.NewGuid().ToString("N"));
    string FilePath => Path.Combine(_dir, "reborn-users.json");

    public UserListTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { } }

    // Collects every log line so a test can look for text that must never appear.
    sealed class CaptureLogger<T> : ILogger<T>
    {
        public readonly List<string> Lines = new();
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
        {
            Lines.Add(formatter(state, exception));
            if (exception != null) Lines.Add(exception.ToString());
        }
    }

    IConfiguration Config(params (string label, string key)[] startKeys)
    {
        var dict = new Dictionary<string, string?> { ["UsersFile"] = FilePath };
        foreach (var (label, key) in startKeys) dict["StartKeys:" + label] = key;
        return new ConfigurationBuilder().AddInMemoryCollection(dict).Build();
    }

    static string Json(bool requireKeys, params (string name, string key, bool create, bool confirm)[] users) =>
        JsonSerializer.Serialize(new
        {
            RequireKeys = requireKeys,
            Users = users.Select(u => new { Name = u.name, Key = u.key, CanCreateGames = u.create, ConfirmName = u.confirm, Added = "2026-10-21T18:00:00Z" }),
        });

    // Writes the file and gives it a new last-write time, as an editor or tools/vm.py would.
    void Write(string json, int secondsLater)
    {
        File.WriteAllText(FilePath, json);
        File.SetLastWriteTimeUtc(FilePath, new DateTime(2026, 10, 21, 18, 0, 0, DateTimeKind.Utc).AddSeconds(secondsLater));
    }

    UserList Open(IConfiguration config, ILogger<UserList>? logger = null) =>
        new(config, logger) { RecheckEvery = TimeSpan.Zero };

    [Fact]
    public void ByKey_finds_the_user_exactly()
    {
        Write(Json(true, ("Sintax", DjKey, true, false), ("Mike", MikeKey, false, false)), 0);
        var users = Open(Config());

        var mike = users.ByKey(MikeKey);
        Assert.NotNull(mike);
        Assert.Equal("Mike", mike!.Name);
        Assert.False(mike.CanCreateGames);
        Assert.False(mike.ConfirmName);
        Assert.Equal(new DateTime(2026, 10, 21, 18, 0, 0, DateTimeKind.Utc), mike.Added.ToUniversalTime());
        Assert.True(users.ByKey(DjKey)!.CanCreateGames);

        Assert.Null(users.ByKey(DjKey.ToLowerInvariant()));   // case matters
        Assert.Null(users.ByKey(DjKey[..^1]));
        Assert.Null(users.ByKey(""));
        Assert.Null(users.ByKey(null));
        Assert.Equal(2, users.Count);
        Assert.True(users.RequireKeys);
        Assert.True(users.Exists);
    }

    [Fact]
    public void No_file_means_no_users()
    {
        var users = Open(Config());
        Assert.False(users.Exists);
        Assert.Equal(0, users.Count);
        Assert.Null(users.ByKey(DjKey));
    }

    [Fact]
    public void Changes_to_the_file_apply_without_a_restart()
    {
        Write(Json(true, ("Sintax", DjKey, true, false)), 0);
        var users = Open(Config());
        Assert.NotNull(users.ByKey(DjKey));

        Write(Json(false, ("Mike", MikeKey, true, false)), 5);

        Assert.Null(users.ByKey(DjKey));
        Assert.Equal("Mike", users.ByKey(MikeKey)!.Name);
        Assert.False(users.RequireKeys);
        Assert.Equal(1, users.Count);
    }

    [Fact]
    public void Bad_reread_keeps_the_last_good_list()
    {
        Write(Json(true, ("Sintax", DjKey, true, false)), 0);
        var log = new CaptureLogger<UserList>();
        var users = Open(Config(), log);

        Write("{ this is not json", 5);
        Assert.Equal("Sintax", users.ByKey(DjKey)!.Name);
        Assert.Equal(1, users.Count);
        Assert.True(users.RequireKeys);

        // A second look at the same bad file stays quiet; the warning is logged once.
        Write("{ still not json", 10);
        Assert.NotNull(users.ByKey(DjKey));
        Assert.Single(log.Lines, l => l.Contains("User list unreadable; keeping the previous list"));

        // A wrong shape (no Users array) counts as bad too.
        Write("{\"RequireKeys\": false}", 15);
        Assert.NotNull(users.ByKey(DjKey));
        Assert.True(users.RequireKeys);

        // Once the file is fixed the new list applies.
        Write(Json(true, ("Mike", MikeKey, false, false)), 20);
        Assert.Null(users.ByKey(DjKey));
        Assert.NotNull(users.ByKey(MikeKey));
    }

    // The shared list of damaged files: the game server (reborn/tests/test_access_rules.cpp) and
    // tools/vm.py (debugloop/tests/test_vm.py) refuse exactly these too.
    public static TheoryData<string> BadShapes() => new()
    {
        "",
        "not json",
        "{ \"Users\": [ ",
        "[]",
        "42",
        "{}",
        "{\"RequireKeys\": true}",
        "{\"Users\": {}}",
        "{\"Users\": \"x\"}",
        "{\"Users\": null}",
        "{\"RequireKeys\": \"yes\", \"Users\": []}",
        "{\"RequireKeys\": \"true\", \"Users\": []}",
        "{\"RequireKeys\": 1, \"Users\": []}",
        "{\"RequireKeys\": null, \"Users\": []}",
        "{\"Users\": [5]}",
        "{\"Users\": [{\"Key\": \"K\"}]}",
        "{\"Users\": [{\"Name\": \"A\"}]}",
        "{\"Users\": [{\"Name\": \"\", \"Key\": \"K\"}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"\"}]}",
        "{\"Users\": [{\"Name\": 1, \"Key\": \"K\"}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": 7}]}",
        "{\"Users\": [{\"Name\": null, \"Key\": \"K\"}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"K\", \"CanCreateGames\": \"true\"}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"K\", \"CanCreateGames\": 1}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"K\", \"CanCreateGames\": null}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"K\", \"ConfirmName\": \"false\"}]}",
        "{\"Users\": [{\"Name\": \"A\", \"Key\": \"K\", \"ConfirmName\": 0}]}",
    };

    [Theory]
    [MemberData(nameof(BadShapes))]
    public void A_damaged_file_is_unreadable(string text)
    {
        // First read bad: the file is there, but nobody is let in.
        Write(text, 0);
        var users = Open(Config());
        Assert.True(users.Exists);
        Assert.Equal(0, users.Count);
        Assert.Null(users.ByKey("K"));

        // Later read bad: the last good list stays.
        Write(Json(true, ("Sintax", DjKey, true, false)), 5);
        Assert.NotNull(users.ByKey(DjKey));
        Write(text, 10);
        Assert.True(users.ByKey(DjKey)!.CanCreateGames);
    }

    [Fact]
    public void Optional_settings_have_their_defaults_and_good_shapes_are_read()
    {
        Write("﻿{\"Users\": [{\"Name\": \"Bo\", \"Key\": \"K2\"}, {\"Name\": \"Ann\", \"Key\": \"K1\", \"CanCreateGames\": true, \"ConfirmName\": true, \"Extra\": 1}]}", 0);
        var users = Open(Config());
        Assert.True(users.RequireKeys);
        Assert.False(users.ByKey("K2")!.CanCreateGames);
        Assert.False(users.ByKey("K2")!.ConfirmName);
        Assert.True(users.ByKey("K1")!.CanCreateGames);
        Assert.True(users.ByKey("K1")!.ConfirmName);

        Write("{\"RequireKeys\": false, \"Users\": []}", 5);
        Assert.False(users.RequireKeys);
        Assert.Equal(0, users.Count);
    }

    [Fact]
    public void Migrate_says_to_delete_the_start_keys_afterwards()
    {
        var log = new CaptureLogger<UserList>();
        Assert.True(UserList.Migrate(Config(("dj", DjKey)), FilePath, log));
        Assert.Single(log.Lines, l => l.Contains("delete the StartKeys block from appsettings.json"));

        // Nothing to say when nothing was migrated.
        var quiet = new CaptureLogger<UserList>();
        Assert.False(UserList.Migrate(Config(("dj", DjKey)), FilePath, quiet));
        Assert.DoesNotContain(quiet.Lines, l => l.Contains("StartKeys"));
    }

    [Fact]
    public void Migrate_turns_start_keys_into_users_that_confirm_their_name()
    {
        var config = Config(("dj", DjKey), ("kaz", MikeKey));

        Assert.True(UserList.Migrate(config, FilePath, null));

        var users = Open(config);
        Assert.Equal(2, users.Count);
        Assert.True(users.RequireKeys);
        var dj = users.ByKey(DjKey)!;
        Assert.Equal("dj", dj.Name);
        Assert.True(dj.CanCreateGames);
        Assert.True(dj.ConfirmName);
        Assert.True(Math.Abs((DateTime.UtcNow - dj.Added.ToUniversalTime()).TotalMinutes) < 5);
        var kaz = users.ByKey(MikeKey)!;
        Assert.Equal("kaz", kaz.Name);
        Assert.True(kaz.CanCreateGames);
        Assert.True(kaz.ConfirmName);
        Assert.False(File.Exists(FilePath + ".tmp"));
    }

    [Fact]
    public void Migrate_writes_nothing_without_start_keys()
    {
        var log = new CaptureLogger<UserList>();
        Assert.False(UserList.Migrate(Config(), FilePath, log));
        Assert.False(File.Exists(FilePath));
        Assert.Contains(log.Lines, l => l.Contains("No user list: anyone can join, nobody can start games"));
    }

    [Fact]
    public void Migrate_never_overwrites_an_existing_file()
    {
        string original = Json(false, ("Sintax", DjKey, false, false));
        Write(original, 0);

        Assert.False(UserList.Migrate(Config(("dj", MikeKey)), FilePath, null));

        Assert.Equal(original, File.ReadAllText(FilePath));
    }

    [Fact]
    public void Logs_never_contain_a_key()
    {
        var log = new CaptureLogger<UserList>();
        var config = Config(("dj", DjKey), ("kaz", MikeKey));

        UserList.Migrate(config, FilePath, log);            // migrate
        var users = Open(config, log);                      // load
        users.ByKey(DjKey);
        Write(Json(true, ("Sintax", DjKey, true, false)), 5);   // reload
        users.ByKey(DjKey);
        Write("{ " + MikeKey + " broken", 10);              // bad reload, with a key in the damage
        users.ByKey(DjKey);

        Assert.NotEmpty(log.Lines);
        foreach (string line in log.Lines)
        {
            Assert.DoesNotContain(DjKey, line);
            Assert.DoesNotContain(MikeKey, line);
        }
    }
}
