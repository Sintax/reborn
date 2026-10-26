using System.Text.Json;

namespace Reborn.Update.Tests;

public sealed class InstallerTests : IDisposable
{
    static readonly DateTime When = new(2026, 10, 26, 12, 30, 0, DateTimeKind.Utc);
    static readonly string[] Protected =
    {
        @"matchmaking\appsettings.json", @"matchmaking\admin-token.txt",
        "reborn-users.json", "reborn-users-seen.json", "reborn-zips.json",
    };

    readonly Scratch _s = new();
    readonly FakeServers _servers;
    readonly Installer _installer;

    public InstallerTests()
    {
        _servers = new FakeServers(_s.Win64);
        _installer = new Installer(_s.Win64, _servers, () => When);
        // What is on the server now: version 1.0.1 plus the owner's own files.
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        Scratch.Write(_s.Win64, @"matchmaking\gamecontroller.exe", "old mm");
        Scratch.Write(_s.Win64, "keep.txt", "not part of the update");
        foreach (string p in Protected) Scratch.Write(_s.Win64, p, "owner's " + p);
    }

    public void Dispose() => _s.Dispose();

    string StageVersion(string version, string dll)
    {
        string dir = _s.Staged(version);
        Scratch.Write(dir, "reborn.dll", dll);
        Scratch.Write(dir, @"matchmaking\gamecontroller.exe", "mm " + version);
        Scratch.Write(dir, "new.txt", "added by " + version);
        Scratch.Write(dir, "reborn-version.txt", version);
        return dir;
    }

    string Saved(string version, string relative) => Path.Combine(_s.Versions, version, relative);
    string InProgress => Path.Combine(_s.Versions, "in-progress.json");

    JsonElement LastUpdate() =>
        JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json"))).RootElement;

    void AssertOldVersionBack()
    {
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Equal("old mm", _s.Read(@"matchmaking\gamecontroller.exe"));
        Assert.False(_s.Has("new.txt"));
        Assert.False(_s.Has("reborn-version.txt"));
        Assert.Equal("not part of the update", _s.Read("keep.txt"));
        Assert.Equal("1.0.1\n", _s.Read("reborn-deployed.txt"));
        Assert.False(File.Exists(InProgress));
    }

    [Fact]
    public void Install_replaces_files_and_keeps_protected_ones()
    {
        string staged = StageVersion("1.0.2", "new dll");
        foreach (string p in Protected) Scratch.Write(staged, p, "WRONG " + p);

        InstallResult result = _installer.Install(staged, "1.0.2");

        Assert.True(result.Ok, result.Message);
        Assert.Contains("1.0.2", result.Message);
        Assert.Equal("new dll", _s.Read("reborn.dll"));
        Assert.Equal("mm 1.0.2", _s.Read(@"matchmaking\gamecontroller.exe"));
        Assert.Equal("added by 1.0.2", _s.Read("new.txt"));
        Assert.Equal("1.0.2", _s.Read("reborn-version.txt"));
        Assert.Equal("not part of the update", _s.Read("keep.txt"));
        foreach (string p in Protected) Assert.Equal("owner's " + p, _s.Read(p));
        Assert.Equal("1.0.2", _s.Read("reborn-deployed.txt").Trim());
        Assert.False(File.Exists(InProgress));

        // The backup holds exactly what was replaced, the old version text and the list of files the update added.
        Assert.Equal("old dll", File.ReadAllText(Saved("1.0.1", "reborn.dll")));
        Assert.Equal("old mm", File.ReadAllText(Saved("1.0.1", @"matchmaking\gamecontroller.exe")));
        Assert.Equal("1.0.1\n", File.ReadAllText(Saved("1.0.1", ".reborn-backup.txt")));
        Assert.Equal(new[] { "new.txt", "reborn-version.txt" },
            File.ReadAllLines(Saved("1.0.1", ".reborn-added.txt")).OrderBy(l => l, StringComparer.Ordinal));
        Assert.False(File.Exists(Saved("1.0.1", "new.txt")));
        Assert.False(File.Exists(Saved("1.0.1", "keep.txt")));
        Assert.False(File.Exists(Saved("1.0.1", "reborn-version.txt")));
        foreach (string p in Protected) Assert.False(File.Exists(Saved("1.0.1", p)), p);

        // Safety order: the servers stop before any file changes; matchmaking starts on the new files.
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame" }, _servers.Calls);
        Assert.Equal("old dll", _servers.First("StopAll").Dll);
        Assert.Equal("1.0.1\n", _servers.First("StopAll").Deployed);
        Assert.True(File.Exists(Saved("1.0.1", "reborn.dll")));
        Assert.Equal("new dll", _servers.First("StartMatchmaking").Dll);
        Assert.Equal("1.0.2", _servers.First("StartMatchmaking").Deployed?.Trim());
        Assert.Equal(TimeSpan.FromSeconds(180), _servers.LastTimeout);

        JsonElement last = LastUpdate();
        Assert.Equal("1.0.2", last.GetProperty("version").GetString());
        Assert.True(last.GetProperty("ok").GetBoolean());
        Assert.Equal(result.Message, last.GetProperty("message").GetString());
        Assert.Equal("", last.GetProperty("details").GetString());
        Assert.Equal(When, last.GetProperty("when").GetDateTime().ToUniversalTime());
    }

    [Fact]
    public void failed_health_check_restores_old_version()
    {
        string staged = StageVersion("1.0.2", "new dll");
        _servers.WaitAnswers.Enqueue(false);

        InstallResult result = _installer.Install(staged, "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("1.0.1", result.Message);
        AssertOldVersionBack();
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame", "StopAll", "StartMatchmaking" }, _servers.Calls);
        Assert.Equal("new dll", _servers.Snapshots[3].Dll);      // stopped the new version before restoring
        Assert.Equal("old dll", _servers.Snapshots[4].Dll);      // restarted on the old files
        Assert.Equal("1.0.1\n", _servers.Snapshots[4].Deployed);

        JsonElement last = LastUpdate();
        Assert.Equal("1.0.2", last.GetProperty("version").GetString());
        Assert.False(last.GetProperty("ok").GetBoolean());
    }

    [Fact]
    public void Matchmaking_that_will_not_start_restores_old_version()
    {
        _servers.StartAnswers.Enqueue(false);

        InstallResult result = _installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("matchmaking didn't start", result.Message);
        AssertOldVersionBack();
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "StopAll", "StartMatchmaking" }, _servers.Calls);
        Assert.Equal("old dll", _servers.Snapshots[3].Dll);
    }

    [Fact]
    public void Servers_that_will_not_stop_change_nothing()
    {
        _servers.StopThrows = 1;

        InstallResult result = _installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("stopped", result.Message);
        AssertOldVersionBack();
        Assert.Equal(new[] { "StopAll", "StartMatchmaking" }, _servers.Calls);
        Assert.Contains("Serverborn.exe would not close.", LastUpdate().GetProperty("details").GetString());
    }

    [Fact]
    public void Copy_failure_after_stop_restores_and_restarts()
    {
        string staged = StageVersion("1.0.2", "new dll");
        Scratch.Write(staged, @"fresh\deep\file.txt", "in new folders");
        // A folder sits where the update wants to put a file, so that copy fails part way.
        Scratch.Write(staged, "zz-blocked.txt", "cannot land");
        Directory.CreateDirectory(Path.Combine(_s.Win64, "zz-blocked.txt"));

        InstallResult result = _installer.Install(staged, "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("zz-blocked.txt", result.Message);
        AssertOldVersionBack();
        Assert.False(Directory.Exists(Path.Combine(_s.Win64, "fresh")));
        Assert.True(Directory.Exists(Path.Combine(_s.Win64, "zz-blocked.txt")));
        Assert.Equal(new[] { "StopAll", "StartMatchmaking" }, _servers.Calls);

        JsonElement last = LastUpdate();
        Assert.False(last.GetProperty("ok").GetBoolean());
        string details = last.GetProperty("details").GetString()!;
        Assert.Contains("zz-blocked.txt", details);
        Assert.Contains("error", details, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Roll_back_installs_a_kept_version()
    {
        Assert.True(_installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2").Ok);
        _servers.Clear();

        InstallResult result = _installer.RollBack("1.0.1");

        Assert.True(result.Ok, result.Message);
        Assert.Contains("1.0.1", result.Message);
        AssertOldVersionBack();
        foreach (string p in Protected) Assert.Equal("owner's " + p, _s.Read(p));
        // The version we left is kept so it can be put back again.
        Assert.Equal("new dll", File.ReadAllText(Saved("1.0.2", "reborn.dll")));
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame" }, _servers.Calls);
        Assert.Equal("new dll", _servers.First("StopAll").Dll);
        Assert.Equal("1.0.1", LastUpdate().GetProperty("version").GetString());

        // Nothing kept under that name, or the version already installed: nothing changes.
        _servers.Clear();
        Assert.False(_installer.RollBack("1.0.9").Ok);
        Assert.False(_installer.RollBack("1.0.1").Ok);
        Assert.False(_installer.RollBack(@"..\Win64").Ok);
        Assert.Empty(_servers.Calls);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
    }

    [Fact]
    public void Roll_back_removes_files_added_since_and_forward_puts_them_back()
    {
        Assert.True(_installer.Install(StageVersion("1.0.2", "dll 2"), "1.0.2").Ok);
        string staged3 = StageVersion("1.0.3", "dll 3");
        Scratch.Write(staged3, @"extra\new3.txt", "added by 1.0.3");
        Assert.True(_installer.Install(staged3, "1.0.3").Ok);

        Assert.True(_installer.RollBack("1.0.1").Ok);
        AssertOldVersionBack();
        Assert.False(_s.Has(@"extra\new3.txt"));

        Assert.True(_installer.RollBack("1.0.3").Ok);
        Assert.Equal("dll 3", _s.Read("reborn.dll"));
        Assert.Equal("added by 1.0.3", _s.Read("new.txt"));
        Assert.Equal("added by 1.0.3", _s.Read(@"extra\new3.txt"));
        Assert.Equal("1.0.3", _s.Read("reborn-version.txt"));
        Assert.Equal("1.0.3", _s.Read("reborn-deployed.txt").Trim());
        foreach (string p in Protected) Assert.Equal("owner's " + p, _s.Read(p));
    }

    [Fact]
    public void Deployed_text_comes_back_exactly()
    {
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "2026-10-22 bab2c4e\n");
        Assert.True(_installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2").Ok);
        Assert.Equal(new[] { "2026-10-22-bab2c4e" }, _installer.Kept());

        Assert.True(_installer.RollBack("2026-10-22-bab2c4e").Ok);
        Assert.Equal("2026-10-22 bab2c4e\n", _s.Read("reborn-deployed.txt"));
        Assert.Equal("old dll", _s.Read("reborn.dll"));
    }

    [Fact]
    public void Missing_deployed_file_is_saved_as_unknown_and_stays_missing_on_restore()
    {
        File.Delete(Path.Combine(_s.Win64, "reborn-deployed.txt"));
        _servers.WaitAnswers.Enqueue(false);

        InstallResult result = _installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("previous version was put back", result.Message);
        Assert.True(File.Exists(Saved("unknown", "reborn.dll")));
        Assert.False(_s.Has("reborn-deployed.txt"));
        Assert.Equal("old dll", _s.Read("reborn.dll"));
    }

    [Fact]
    public void Existing_backup_of_a_release_keeps_its_files()
    {
        // A copy of 1.0.1 already exists; the live reborn.dll is not 1.0.1's (an earlier run broke off).
        Scratch.Write(Path.Combine(_s.Versions, "1.0.1"), "reborn.dll", "old dll");
        Scratch.Write(Path.Combine(_s.Versions, "1.0.1"), ".reborn-backup.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "half-new mix");

        Assert.True(_installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2").Ok);

        Assert.Equal("old dll", File.ReadAllText(Saved("1.0.1", "reborn.dll")));
        Assert.Equal("old mm", File.ReadAllText(Saved("1.0.1", @"matchmaking\gamecontroller.exe")));
    }

    [Fact]
    public void Unfinished_update_must_be_rolled_back_first()
    {
        // 1.0.1 -> 1.0.2 broke off half way: the marker is left, files are mixed, the version file says 1.0.1.
        Scratch.Write(Path.Combine(_s.Versions, "1.0.1"), "reborn.dll", "old dll");
        Scratch.Write(Path.Combine(_s.Versions, "1.0.1"), ".reborn-backup.txt", "1.0.1\n");
        Scratch.Write(Path.Combine(_s.Versions, "1.0.1"), ".reborn-added.txt", "new.txt\n");
        Scratch.Write(Path.Combine(_s.Versions, "1.0.0x"), ".reborn-backup.txt", "1.0.0x\n");
        File.WriteAllText(InProgress, """{"from":"1.0.1","to":"1.0.2","when":"2026-10-26T12:00:00Z"}""");
        Scratch.Write(_s.Win64, "reborn.dll", "half-new mix");
        Scratch.Write(_s.Win64, "new.txt", "added by 1.0.2");

        InstallResult refused = _installer.Install(StageVersion("1.0.3", "dll 3"), "1.0.3");
        Assert.False(refused.Ok);
        Assert.Equal("A previous update didn't finish. Roll back to 1.0.1 first.", refused.Message);
        Assert.False(_installer.RollBack("1.0.0x").Ok);
        Assert.Empty(_servers.Calls);
        Assert.Equal("half-new mix", _s.Read("reborn.dll"));

        InstallResult result = _installer.RollBack("1.0.1");

        Assert.True(result.Ok, result.Message);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.False(_s.Has("new.txt"));
        Assert.False(File.Exists(InProgress));
        Assert.Equal("old dll", File.ReadAllText(Saved("1.0.1", "reborn.dll")));
        Assert.True(_installer.Install(StageVersion("1.0.3", "dll 3"), "1.0.3").Ok);
    }

    [Fact]
    public void Only_three_versions_are_kept()
    {
        Directory.CreateDirectory(Path.Combine(_s.Versions, "staging", "1.0.9"));
        for (int build = 2; build <= 5; build++)
            Assert.True(_installer.Install(StageVersion($"1.0.{build}", $"dll {build}"), $"1.0.{build}").Ok);

        Assert.Equal(new[] { "1.0.4", "1.0.3", "1.0.2" }, _installer.Kept());
        Assert.False(Directory.Exists(Path.Combine(_s.Versions, "1.0.1")));
        Assert.True(Directory.Exists(Path.Combine(_s.Versions, "staging")));

        // 1.0.5 -> 1.0.4: the installed version's own copy doesn't count toward the three.
        Assert.True(_installer.RollBack("1.0.4").Ok);
        _installer.Prune();
        Assert.Equal(new[] { "1.0.5", "1.0.3", "1.0.2" }, _installer.Kept());
        Assert.True(Directory.Exists(Path.Combine(_s.Versions, "1.0.4")));

        // Back to 1.0.2, then forward to 1.0.6: the copy of 1.0.2 (what ran just before) is kept
        // even though it is the oldest build.
        Assert.True(_installer.RollBack("1.0.2").Ok);
        Assert.True(_installer.Install(StageVersion("1.0.6", "dll 6"), "1.0.6").Ok);
        Assert.Equal(new[] { "1.0.2", "1.0.4", "1.0.5" }, _installer.Kept());

        _installer.Prune(keep: 1);
        Assert.Equal(new[] { "1.0.2" }, _installer.Kept());
        Assert.True(File.Exists(Path.Combine(_s.Versions, "last-update.json")));
    }

    [Fact]
    public void Bad_input_changes_nothing()
    {
        Assert.False(_installer.Install(Path.Combine(_s.Root, "missing"), "1.0.2").Ok);
        Assert.False(_installer.Install(StageVersion("1.0.2", "new dll"), "7984e6e").Ok);
        Assert.False(_installer.Install(_s.Staged("empty"), "1.0.2").Ok);
        Assert.Empty(_servers.Calls);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Equal("1.0.1", _s.Read("reborn-deployed.txt").Trim());
        Assert.False(LastUpdate().GetProperty("ok").GetBoolean());
    }
}
