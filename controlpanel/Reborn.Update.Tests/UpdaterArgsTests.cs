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
        var servers = new FakeServers(_s.Win64);
        var started = new List<string>();
        var output = new StringWriter();

        int exit = UpdaterCli.Run(["rollback", "--to", "1.0.9", "--win64", _s.Win64, "--panel-pid", "9", "--panel", "p.exe"],
            output, servers, (_, _) => true, (exe, _) => { started.Add(exe); return true; }, () => When);

        Assert.Equal(1, exit);
        Assert.Empty(servers.Calls);
        Assert.Equal(["p.exe"], started);
        using JsonDocument last = JsonDocument.Parse(File.ReadAllText(Path.Combine(_s.Versions, "last-update.json")));
        Assert.False(last.RootElement.GetProperty("ok").GetBoolean());
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
    [InlineData("[{\"name\":\"Story\"}]", true)]
    [InlineData("[]", false)]
    [InlineData("{}", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    [InlineData("<html>", false)]
    public void A_game_counts_only_when_the_list_has_an_entry(string? json, bool expected) =>
        Assert.Equal(expected, WindowsServerControl.HasGame(json));
}
