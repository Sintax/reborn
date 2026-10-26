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
    readonly FakeServers _servers = new();
    readonly Installer _installer;

    public InstallerTests()
    {
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

    JsonElement LastUpdate() =>
        JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json"))).RootElement;

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
        Assert.Equal("not part of the update", _s.Read("keep.txt"));
        foreach (string p in Protected) Assert.Equal("owner's " + p, _s.Read(p));
        Assert.Equal("1.0.2", _s.Read("reborn-deployed.txt").Trim());

        // The backup holds exactly what was replaced, plus the old version number.
        string backup = Path.Combine(_s.Versions, "1.0.1");
        Assert.Equal("old dll", File.ReadAllText(Path.Combine(backup, "reborn.dll")));
        Assert.Equal("old mm", File.ReadAllText(Path.Combine(backup, @"matchmaking\gamecontroller.exe")));
        Assert.Equal("1.0.1", File.ReadAllText(Path.Combine(backup, "reborn-version.txt")).Trim());
        Assert.False(File.Exists(Path.Combine(backup, "new.txt")));
        Assert.False(File.Exists(Path.Combine(backup, "keep.txt")));
        foreach (string p in Protected) Assert.False(File.Exists(Path.Combine(backup, p)), p);

        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame" }, _servers.Calls);
        Assert.Equal(TimeSpan.FromSeconds(180), _servers.LastTimeout);

        JsonElement last = LastUpdate();
        Assert.Equal("1.0.2", last.GetProperty("version").GetString());
        Assert.True(last.GetProperty("ok").GetBoolean());
        Assert.Equal(result.Message, last.GetProperty("message").GetString());
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
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Equal("old mm", _s.Read(@"matchmaking\gamecontroller.exe"));
        Assert.False(_s.Has("new.txt"));
        Assert.Equal("not part of the update", _s.Read("keep.txt"));
        Assert.Equal("1.0.1", _s.Read("reborn-deployed.txt").Trim());
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame", "StopAll", "StartMatchmaking" }, _servers.Calls);

        JsonElement last = LastUpdate();
        Assert.Equal("1.0.2", last.GetProperty("version").GetString());
        Assert.False(last.GetProperty("ok").GetBoolean());
    }

    [Fact]
    public void Copy_failure_after_stop_restores_and_restarts()
    {
        string staged = StageVersion("1.0.2", "new dll");
        // A folder sits where the update wants to put a file, so that copy fails part way.
        Scratch.Write(staged, "zz-blocked.txt", "cannot land");
        Directory.CreateDirectory(Path.Combine(_s.Win64, "zz-blocked.txt"));

        InstallResult result = _installer.Install(staged, "1.0.2");

        Assert.False(result.Ok);
        Assert.Contains("zz-blocked.txt", result.Message);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Equal("old mm", _s.Read(@"matchmaking\gamecontroller.exe"));
        Assert.False(_s.Has("new.txt"));
        Assert.Equal("1.0.1", _s.Read("reborn-deployed.txt").Trim());
        Assert.Equal(new[] { "StopAll", "StartMatchmaking" }, _servers.Calls);
        Assert.False(LastUpdate().GetProperty("ok").GetBoolean());
    }

    [Fact]
    public void Roll_back_installs_a_kept_version()
    {
        Assert.True(_installer.Install(StageVersion("1.0.2", "new dll"), "1.0.2").Ok);
        _servers.Calls.Clear();

        InstallResult result = _installer.RollBack("1.0.1");

        Assert.True(result.Ok, result.Message);
        Assert.Contains("1.0.1", result.Message);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Equal("old mm", _s.Read(@"matchmaking\gamecontroller.exe"));
        Assert.Equal("1.0.1", _s.Read("reborn-deployed.txt").Trim());
        foreach (string p in Protected) Assert.Equal("owner's " + p, _s.Read(p));
        // The version we left is kept so it can be put back again.
        Assert.Equal("new dll", File.ReadAllText(Path.Combine(_s.Versions, "1.0.2", "reborn.dll")));
        Assert.Equal(new[] { "StopAll", "StartMatchmaking", "WaitForGame" }, _servers.Calls);
        Assert.Equal("1.0.1", LastUpdate().GetProperty("version").GetString());

        // Nothing kept under that name, or the version already installed: nothing changes.
        _servers.Calls.Clear();
        Assert.False(_installer.RollBack("1.0.9").Ok);
        Assert.False(_installer.RollBack("1.0.1").Ok);
        Assert.False(_installer.RollBack(@"..\Win64").Ok);
        Assert.Empty(_servers.Calls);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
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

        // Back to 1.0.2, then forward to 1.0.6: the copy of 1.0.2 (what ran just before) is kept
        // even though it is the oldest build.
        Assert.True(_installer.RollBack("1.0.2").Ok);
        Assert.Equal(new[] { "1.0.5", "1.0.4", "1.0.3" }, _installer.Kept());
        Assert.True(_installer.Install(StageVersion("1.0.6", "dll 6"), "1.0.6").Ok);
        Assert.Equal(new[] { "1.0.2", "1.0.5", "1.0.4" }, _installer.Kept());

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
