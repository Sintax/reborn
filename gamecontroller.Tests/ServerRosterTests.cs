using gamecontroller.Models;
using gamecontroller.Singletons;

namespace gamecontroller.Tests;

// A launcher that only remembers what it was asked to start; nothing really runs.
class FakeLauncher : IServerLauncher
{
    public List<(string Instance, string CommandLine)> Launched = new();
    public HashSet<int> Running = new();
    public List<int> Killed = new();
    int _nextPid = 100;

    public int Launch(string instanceName, string commandLine)
    {
        Launched.Add((instanceName, commandLine));
        int pid = _nextPid++;
        Running.Add(pid);
        return pid;
    }

    public bool IsRunning(int pid) => Running.Contains(pid);

    public void Kill(int pid)
    {
        Killed.Add(pid);
        Running.Remove(pid);
    }
}

public class ServerRosterTests
{
    static readonly RosterSlot Algorithm = new("Algorithm", "Story - The Algorithm", "open Caverns_P", 5);
    static readonly RosterSlot Overgrowth = new("Overgrowth", "Versus - Incursion Overgrowth", "open Overgrowth_P", 10);
    static readonly DateTime T0 = new(2026, 10, 10, 12, 0, 0, DateTimeKind.Utc);

    static (ServerRoster, FakeLauncher) Make(params RosterSlot[] slots)
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(slots, launcher, new[] { 7777, 7778, 7779 }, "localhost:5000");
        roster.PublicIp = "216.226.149.110";
        return (roster, launcher);
    }

    [Fact]
    public void Reconcile_launches_one_server_per_roster_slot()
    {
        var (roster, launcher) = Make(Algorithm, Overgrowth);

        roster.Reconcile(T0);

        Assert.Equal(2, launcher.Launched.Count);
        Assert.Equal(2, roster.Instances.Count);
        Assert.Equal(new[] { 7777, 7778 }, roster.Instances.Select(i => i.Port));
        Assert.Equal(new[] { "rb-algorithm", "rb-overgrowth" }, launcher.Launched.Select(l => l.Instance));
        // Map and player count are not on the command line: the server asks the coordinator for them.
        Assert.All(roster.Instances, i => Assert.Contains(launcher.Launched, l =>
            l.CommandLine == "-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbinstance=rb-" + i.Slot!.Name.ToLower()
                + " -rbcoordinator=localhost:5000 -rbcoordinatorkey=" + i.MyGuid));
    }

    [Fact]
    public void Reconcile_leaves_a_live_slot_alone()
    {
        var (roster, launcher) = Make(Algorithm);
        roster.Reconcile(T0);

        roster.Reconcile(T0.AddSeconds(5));

        Assert.Single(launcher.Launched);
    }

    [Fact]
    public void Reconcile_waits_for_public_ip()
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(new[] { Algorithm }, launcher, new[] { 7777 }, "localhost:5000");

        roster.Reconcile(T0);

        Assert.Empty(launcher.Launched);
    }

    [Fact]
    public void Reconcile_relaunches_a_slot_whose_process_died()
    {
        var (roster, launcher) = Make(Algorithm);
        roster.Reconcile(T0);
        launcher.Running.Clear();

        roster.Reconcile(T0.AddSeconds(1));

        Assert.Equal(2, launcher.Launched.Count);
        Assert.Single(roster.Instances);
        Assert.Equal(101, roster.Instances[0].PID);
    }

    [Fact]
    public void Reconcile_kills_and_relaunches_a_server_that_stopped_checking_in()
    {
        var (roster, launcher) = Make(Algorithm);
        roster.Reconcile(T0);
        // The game server needs a while to load the map before its first check-in.
        roster.Reconcile(T0.AddSeconds(100));
        Assert.Single(launcher.Launched);
        roster.RecordPoll(roster.Instances[0].MyGuid, 0, false, T0.AddSeconds(100));

        roster.Reconcile(T0.AddSeconds(121));

        Assert.Equal(new[] { 100 }, launcher.Killed);
        Assert.Equal(2, launcher.Launched.Count);
    }

    [Fact]
    public void ConfigFor_answers_the_servers_own_token_only()
    {
        var (roster, _) = Make(Algorithm);
        roster.Reconcile(T0);

        var config = roster.ConfigFor(roster.Instances[0].MyGuid);

        Assert.NotNull(config);
        Assert.Equal("open Caverns_P", config!.ServerStartupCommand);
        Assert.Equal(5, config.MaxNumPlayers);
        Assert.Null(roster.ConfigFor("nope"));
    }

    [Fact]
    public void RecordPoll_updates_players_and_started()
    {
        var (roster, _) = Make(Algorithm);
        roster.Reconcile(T0);
        var token = roster.Instances[0].MyGuid;

        Assert.True(roster.RecordPoll(token, 3, true, T0.AddSeconds(5)));
        Assert.False(roster.RecordPoll("nope", 1, false, T0.AddSeconds(5)));

        var entry = Assert.Single(roster.BrowserEntries());
        Assert.Equal(3, entry.CurrentNumPlayers);
        Assert.True(entry.MatchStarted);
    }

    [Fact]
    public void BrowserEntries_describe_each_server_for_the_game()
    {
        var (roster, _) = Make(Algorithm, Overgrowth);
        roster.Reconcile(T0);

        var entries = roster.BrowserEntries();

        Assert.Equal(2, entries.Count);
        Assert.Equal("Algorithm", entries[0].InstanceName);
        Assert.Equal("Story - The Algorithm", entries[0].HumanReadableInstanceMapMode);
        Assert.Equal(0, entries[0].CurrentNumPlayers);
        Assert.Equal(5, entries[0].MaxNumPlayers);
        Assert.Equal("open 216.226.149.110:7777", entries[0].ServerConnectString);
        Assert.False(entries[0].MatchStarted);
        Assert.Equal("open 216.226.149.110:7778", entries[1].ServerConnectString);
    }

    [Fact]
    public void MarkFinished_makes_the_next_reconcile_start_a_fresh_server()
    {
        var (roster, launcher) = Make(Algorithm);
        roster.Reconcile(T0);
        var token = roster.Instances[0].MyGuid;

        Assert.True(roster.MarkFinished(token));
        roster.Reconcile(T0.AddSeconds(1));

        Assert.Equal(new[] { 100 }, launcher.Killed);
        Assert.Equal(2, launcher.Launched.Count);
        Assert.NotEqual(token, roster.Instances[0].MyGuid);
    }
}
