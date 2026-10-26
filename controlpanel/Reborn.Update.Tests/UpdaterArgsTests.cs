using System.Text.Json;
using RebornUpdater;

namespace Reborn.Update.Tests;

public sealed class UpdaterArgsTests : IDisposable
{
    static readonly DateTime When = new(2026, 10, 26, 12, 30, 0, DateTimeKind.Utc);

    readonly Scratch _s = new();

    public void Dispose() => _s.Dispose();

    static string[] InstallArgs(string win64 = @"C:\g\Win64", string staged = @"C:\t\Win64") =>
        ["install", "--staged", staged, "--version", "1.0.7", "--win64", win64, "--panel-pid", "4242", "--panel", @"C:\g\Win64\ControlPanel\ControlPanel.exe"];

    [Fact]
    public void Args_parse_install_and_rollback()
    {
        UpdaterArgs? install = UpdaterArgs.Parse(InstallArgs());
        Assert.NotNull(install);
        Assert.Equal("install", install!.Command);
        Assert.Equal(@"C:\t\Win64", install.Staged);
        Assert.Equal("1.0.7", install.Version);
        Assert.Equal(@"C:\g\Win64", install.Win64);
        Assert.Equal(4242, install.PanelPid);
        Assert.Equal(@"C:\g\Win64\ControlPanel\ControlPanel.exe", install.Panel);

        // The order of the flags does not matter.
        UpdaterArgs? back = UpdaterArgs.Parse(["rollback", "--panel", "p.exe", "--panel-pid", "7", "--win64", @"C:\g", "--to", "1.0.3"]);
        Assert.NotNull(back);
        Assert.Equal("rollback", back!.Command);
        Assert.Equal("1.0.3", back.Version);
        Assert.Null(back.Staged);
        Assert.Equal(7, back.PanelPid);

        // A saved copy of an old-style deploy has a name like this; the installer decides if it exists.
        Assert.NotNull(UpdaterArgs.Parse(["rollback", "--to", "2026-10-22-bab2c4e", "--win64", @"C:\g", "--panel-pid", "7", "--panel", "p.exe"]));
    }

    public static IEnumerable<object[]> BadLines()
    {
        yield return new object[] { Array.Empty<string>() };
        yield return new object[] { new[] { "update" } };
        yield return new object[] { new[] { "install" } };
        yield return new object[] { new[] { "rollback" } };
        // a flag missing
        yield return new object[] { new[] { "install", "--staged", "s", "--version", "1.0.7", "--win64", "w", "--panel-pid", "4" } };
        // a flag with no value
        yield return new object[] { new[] { "install", "--staged", "s", "--version", "1.0.7", "--win64", "w", "--panel-pid", "4", "--panel" } };
        // the same flag twice
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--to", "1.0.2", "--win64", "w", "--panel-pid", "4", "--panel", "p" } };
        // a flag of the other command
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--staged", "s", "--win64", "w", "--panel-pid", "4", "--panel", "p" } };
        // an unknown flag
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--win64", "w", "--panel-pid", "4", "--panel", "p", "--force", "yes" } };
        // not a release number
        yield return new object[] { new[] { "install", "--staged", "s", "--version", "latest", "--win64", "w", "--panel-pid", "4", "--panel", "p" } };
        yield return new object[] { new[] { "install", "--staged", "s", "--version", "1.0.0", "--win64", "w", "--panel-pid", "4", "--panel", "p" } };
        // bad process numbers
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--win64", "w", "--panel-pid", "abc", "--panel", "p" } };
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--win64", "w", "--panel-pid", "0", "--panel", "p" } };
        yield return new object[] { new[] { "rollback", "--to", "1.0.1", "--win64", "w", "--panel-pid", "-5", "--panel", "p" } };
        // an empty value
        yield return new object[] { new[] { "rollback", "--to", " ", "--win64", "w", "--panel-pid", "4", "--panel", "p" } };
    }

    [Theory]
    [MemberData(nameof(BadLines))]
    public void Bad_args_print_usage_and_exit_2(string[] args)
    {
        Assert.Null(UpdaterArgs.Parse(args));

        var output = new StringWriter();
        var servers = new FakeServers(_s.Win64);
        int exit = UpdaterCli.Run(args, output, servers, (_, _) => throw new Xunit.Sdk.XunitException("must not wait"),
            (_, _) => throw new Xunit.Sdk.XunitException("must not start"), () => When);

        Assert.Equal(2, exit);
        Assert.Contains("RebornUpdater.exe install --staged", output.ToString());
        Assert.Contains("RebornUpdater.exe rollback --to", output.ToString());
        Assert.Empty(servers.Calls);
    }

    [Fact]
    public void Panel_that_will_not_close_changes_nothing_and_says_so()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(_s.Win64);
        var waited = new List<(int Pid, TimeSpan Limit)>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, servers,
            (pid, limit) => { waited.Add((pid, limit)); return false; },
            (_, _) => throw new Xunit.Sdk.XunitException("the panel is still open; it must not be started again"), () => When);

        Assert.Equal(1, exit);
        Assert.Equal([(4242, TimeSpan.FromSeconds(30))], waited);
        Assert.Empty(servers.Calls);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Contains("Close the Control Panel and try again.", output.ToString());

        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
        Assert.Equal("Close the Control Panel and try again.", last.RootElement.GetProperty("message").GetString());
        Assert.Equal("1.0.7", last.RootElement.GetProperty("version").GetString());
    }

    [Fact]
    public void Install_swaps_the_files_then_starts_the_panel_again()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(_s.Win64);
        var events = new List<string>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, servers,
            (_, _) => { events.Add("panel closed"); return true; },
            (exe, dir) => { events.Add("start " + exe); return true; }, () => When);

        Assert.Equal(0, exit);
        Assert.Equal("new dll", _s.Read("reborn.dll"));
        Assert.Equal("1.0.7\n", _s.Read("reborn-deployed.txt"));
        Assert.Equal(["StopAll", "StartMatchmaking", "WaitForGame"], servers.Calls);
        Assert.Equal(["panel closed", @"start C:\g\Win64\ControlPanel\ControlPanel.exe"], events);
        Assert.Contains("Updated to 1.0.7.", output.ToString());
    }

    [Fact]
    public void Failed_install_still_starts_the_panel_and_exits_1()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(_s.Win64);
        servers.WaitAnswers.Enqueue(false);   // no game shows up: the installer puts 1.0.1 back
        var started = new List<string>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, servers, (_, _) => true,
            (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.Single(started);
        Assert.Contains("didn't work", output.ToString());
    }

    [Fact]
    public void Rollback_to_a_version_that_was_not_kept_says_so_and_starts_the_panel()
    {
        MakeGameFolder();
        var servers = new FakeServers(_s.Win64);
        var started = new List<string>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            output, servers, (_, _) => true, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(servers.Calls);
        Assert.Equal([Path.GetFullPath("p.exe")], started);   // handed on as an absolute path
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
    }

    // A pretend game folder: Battleborn.exe is what marks a real Win64.
    void MakeGameFolder() => Scratch.Write(_s.Win64, "Battleborn.exe", "game");

    [Fact]
    public void Missing_win64_folder_changes_nothing_and_exits_1()
    {
        string missing = Path.Combine(_s.Root, "no-such-folder");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(missing);
        var output = new StringWriter();

        var started = new List<string>();

        int exit = UpdaterCli.Run(InstallArgs(missing, staged), output, servers,
            (_, _) => true, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(servers.Calls);
        Assert.False(Directory.Exists(missing));   // nothing is written into a folder that isn't the game's
        Assert.Contains("isn't the game's Win64 folder", output.ToString());
        Assert.Single(started);                    // the panel closed itself for this run: it is opened again
    }

    [Fact]
    public void Win64_folder_without_the_game_changes_nothing_and_exits_1()
    {
        // A folder that exists but is not the game's (no Battleborn.exe, no matchmaking folder).
        Scratch.Write(_s.Win64, "other.txt", "x");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(_s.Win64);
        var output = new StringWriter();

        var started = new List<string>();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, servers,
            (_, _) => true, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(servers.Calls);
        Assert.False(Directory.Exists(_s.Versions));
        Assert.Contains("isn't the game's Win64 folder", output.ToString());
        Assert.Single(started);
    }

    [Fact]
    public void Missing_staged_folder_changes_nothing_and_exits_1()
    {
        MakeGameFolder();
        var servers = new FakeServers(_s.Win64);
        var output = new StringWriter();

        var started = new List<string>();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, Path.Combine(_s.Root, "no-staged")), output, servers,
            (_, _) => true, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(servers.Calls);
        Assert.Contains("downloaded update folder is missing", output.ToString());
        Assert.Single(started);
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
        Assert.Equal("The downloaded update folder is missing. Nothing was changed.", last.RootElement.GetProperty("message").GetString());
        Assert.Equal("1.0.7", last.RootElement.GetProperty("version").GetString());
    }

    [Fact]
    public void A_panel_still_open_after_a_folder_problem_is_not_started_twice()
    {
        var started = new List<string>();

        int exit = UpdaterCli.Run(InstallArgs(Path.Combine(_s.Root, "no-such-folder"), _s.Staged("1.0.7")), new StringWriter(),
            new FakeServers(_s.Win64), (_, _) => false, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(started);
    }

    [Fact]
    public void A_good_install_deletes_the_unpacked_download()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staging = Path.Combine(_s.Versions, "staging");
        string staged = Path.Combine(staging, "1.0.7", "Win64");
        Scratch.Write(staged, "reborn.dll", "new dll");

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), new StringWriter(), new FakeServers(_s.Win64),
            (_, _) => true, (_, _) => true, () => When);

        Assert.Equal(0, exit);
        Assert.Equal("new dll", _s.Read("reborn.dll"));
        Assert.False(Directory.Exists(staging));
    }

    [Fact]
    public void A_failed_install_keeps_the_unpacked_download_until_the_next_one()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = Path.Combine(_s.Versions, "staging", "1.0.7", "Win64");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var servers = new FakeServers(_s.Win64);
        servers.WaitAnswers.Enqueue(false);

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), new StringWriter(), servers,
            (_, _) => true, (_, _) => true, () => When);

        Assert.Equal(1, exit);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
        Assert.True(Directory.Exists(staged));
    }

    [Fact]
    public void A_mark_that_will_not_go_away_is_a_failure_and_keeps_the_download()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = Path.Combine(_s.Versions, "staging", "1.0.7", "Win64");
        Scratch.Write(staged, "reborn.dll", "new dll");
        string mark = Path.Combine(_s.Versions, "in-progress.json");
        using var servers = new LockMarkServers(mark);
        var output = new StringWriter();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, servers, (_, _) => true, (_, _) => true, () => When);
        servers.Dispose();

        Assert.Equal(1, exit);
        Assert.Equal("new dll", _s.Read("reborn.dll"));
        Assert.True(File.Exists(mark));
        Assert.True(Directory.Exists(staged));
        Assert.Contains("Updated to 1.0.7.", output.ToString());
        Assert.Contains("in-progress mark couldn't be removed", output.ToString());
        Assert.Contains(mark, output.ToString());
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
    }

    // Holds in-progress.json open once the game is waited for, so the installer can't delete it.
    sealed class LockMarkServers(string mark) : IServerControl, IDisposable
    {
        FileStream? _lock;
        public void StopAll() { }
        public bool StartMatchmaking() => true;
        public bool WaitForGame(TimeSpan timeout)
        {
            _lock ??= new FileStream(mark, FileMode.Open, FileAccess.Read, FileShare.Read);
            return true;
        }
        public void Dispose()
        {
            _lock?.Dispose();
            _lock = null;
        }
    }

    [Fact]
    public void A_matchmaking_folder_is_enough_to_count_as_the_game_folder()
    {
        Directory.CreateDirectory(Path.Combine(_s.Win64, "matchmaking"));
        var servers = new FakeServers(_s.Win64);
        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            new StringWriter(), servers, (_, _) => true, (_, _) => true, () => When);
        Assert.Equal(1, exit);   // got past the folder check; 1.0.9 was simply never saved
        Assert.True(Directory.Exists(_s.Versions));
    }

    [Fact]
    public void Relative_paths_are_made_absolute_before_the_panel_is_started()
    {
        MakeGameFolder();
        var started = new List<(string Exe, string Dir)>();
        string relative = Path.GetRelativePath(Directory.GetCurrentDirectory(), _s.Win64);

        UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", relative, "--panel-pid", "9", "--panel", Path.Combine(relative, "ControlPanel", "ControlPanel.exe")],
            new StringWriter(), new FakeServers(_s.Win64), (_, _) => true, (exe, dir) => { started.Add((exe, dir)); return true; }, () => When);

        Assert.Single(started);
        Assert.True(Path.IsPathRooted(started[0].Exe));
        Assert.Equal(Path.Combine(_s.Win64, "ControlPanel", "ControlPanel.exe"), started[0].Exe);
        Assert.Equal(Path.Combine(_s.Win64, "ControlPanel"), started[0].Dir);
    }

    sealed class ThrowingServers : IServerControl
    {
        public void StopAll() => throw new InvalidOperationException("stop blew up");
        public bool StartMatchmaking() => throw new InvalidOperationException("start blew up");
        public bool WaitForGame(TimeSpan timeout) => throw new InvalidOperationException("wait blew up");
    }

    [Fact]
    public void Servers_that_throw_still_end_with_exit_1_and_the_panel_opened_again()
    {
        MakeGameFolder();
        Scratch.Write(_s.Win64, "reborn-deployed.txt", "1.0.1\n");
        Scratch.Write(_s.Win64, "reborn.dll", "old dll");
        string staged = _s.Staged("1.0.7");
        Scratch.Write(staged, "reborn.dll", "new dll");
        var started = new List<string>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(InstallArgs(_s.Win64, staged), output, new ThrowingServers(), (_, _) => true,
            (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Single(started);
        Assert.Equal("old dll", _s.Read("reborn.dll"));
    }

    [Fact]
    public void An_unexpected_error_prints_one_line_exits_1_and_still_opens_the_panel()
    {
        MakeGameFolder();
        var started = new List<string>();
        var output = new StringWriter();

        // The clock is only read once the panel has closed, so this fails inside the guarded part. The
        // updater's own record still goes in (with the computer's clock).
        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            output, new FakeServers(_s.Win64), (_, _) => true, (exe, _) => { started.Add(exe); return true; },
            () => throw new InvalidOperationException("clock broke"));

        Assert.Equal(1, exit);
        Assert.Contains("The roll back stopped because of an error: clock broke", output.ToString());
        Assert.Single(started);
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
        Assert.StartsWith("The roll back stopped", last.RootElement.GetProperty("message").GetString());
    }

    [Fact]
    public void An_error_while_waiting_for_the_panel_does_not_start_the_panel()
    {
        MakeGameFolder();
        var output = new StringWriter();
        var servers = new FakeServers(_s.Win64);

        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            output, servers, (_, _) => throw new InvalidOperationException("no access"),
            (_, _) => throw new Xunit.Sdk.XunitException("the panel never closed"), () => When);

        Assert.Equal(1, exit);
        Assert.Contains("The updater hit a problem: no access", output.ToString());
        Assert.Empty(servers.Calls);
        // Recorded, so the panel shows it when it is opened again.
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.Equal("The updater hit a problem: no access", last.RootElement.GetProperty("message").GetString());
    }

    [Fact]
    public void An_error_inside_a_roll_back_says_the_roll_back_stopped_and_is_recorded()
    {
        MakeGameFolder();
        int calls = 0;
        var output = new StringWriter();

        // The first clock read is inside the installer; the next one is the updater writing its own record.
        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            output, new FakeServers(_s.Win64), (_, _) => true, (_, _) => true,
            () => calls++ == 0 ? throw new InvalidOperationException("clock broke") : When);

        Assert.Equal(1, exit);
        Assert.Contains("The roll back stopped because of an error: clock broke", output.ToString());
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.StartsWith("The roll back stopped", last.RootElement.GetProperty("message").GetString());
    }

    [Fact]
    public void Failure_wording_says_roll_back_or_update()
    {
        Assert.Equal("The roll back stopped", UpdaterCli.StoppedText(UpdaterArgs.Parse(["rollback", "--to", "1.0.1", "--win64", "w", "--panel-pid", "4", "--panel", "p"])!));
        Assert.Equal("The update stopped", UpdaterCli.StoppedText(UpdaterArgs.Parse(InstallArgs())!));
    }

    [Fact]
    public void Unreadable_matchmaking_note_says_what_to_do()
    {
        Assert.StartsWith("NOTE:", WindowsServerControl.UnreadableNote);
        Assert.Contains("another Windows account", WindowsServerControl.UnreadableNote);
        Assert.Contains("could not be stopped", WindowsServerControl.UnreadableNote);
    }

    [Fact]
    public void Port_name_is_read_in_any_letter_case()
    {
        Scratch.Write(_s.Win64, @"matchmaking\appsettings.json", "{ \"coordinatorport\": 6123 }");
        Assert.Equal(6123, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
        Scratch.Write(_s.Win64, @"matchmaking\appsettings.json", "{ \"COORDINATORPORT\": \"6124\" }");
        Assert.Equal(6124, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
    }

    [Fact]
    public void Port_comes_from_appsettings_or_defaults_to_5000()
    {
        Assert.Equal(5000, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
        Scratch.Write(_s.Win64, @"matchmaking\appsettings.json", "{ \"CoordinatorPort\": \"5123\", /* note */ }");
        Assert.Equal(5123, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
        Scratch.Write(_s.Win64, @"matchmaking\appsettings.json", "{ \"CoordinatorPort\": 6001 }");
        Assert.Equal(6001, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
        Scratch.Write(_s.Win64, @"matchmaking\appsettings.json", "not json");
        Assert.Equal(5000, WindowsServerControl.ReadCoordinatorPort(_s.Win64));
    }

    [Theory]
    [InlineData("[{\"ready\":true}]", true)]
    [InlineData("[{\"ready\":false}]", false)]
    [InlineData("[{\"name\":\"Story\",\"ready\":false},{\"name\":\"PvP\",\"Ready\":true}]", true)]
    [InlineData("[{\"READY\":true}]", true)]
    [InlineData("[{\"name\":\"Story\"}]", false)]
    [InlineData("[{\"ready\":\"true\"}]", false)]
    [InlineData("[true]", false)]
    [InlineData("[]", false)]
    [InlineData("{}", false)]
    [InlineData("{\"ready\":true}", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    [InlineData("<html>", false)]
    public void A_game_counts_only_once_it_is_ready(string? json, bool expected)
    {
        Assert.Equal(expected, GamesList.HasReadyGame(json));
        Assert.Equal(expected, WindowsServerControl.HasGame(json));
    }
}
