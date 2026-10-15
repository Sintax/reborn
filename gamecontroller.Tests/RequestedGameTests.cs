using gamecontroller.Models;
using gamecontroller.Singletons;

namespace gamecontroller.Tests;

public class RequestedGameTests
{
    static readonly RosterSlot Algorithm = new("Algorithm", "Story - The Algorithm", "open Caverns_P", 2);
    static readonly DateTime T0 = new(2026, 10, 15, 12, 0, 0, DateTimeKind.Utc);

    static (ServerRoster, FakeLauncher) Make(params RosterSlot[] slots)
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(slots, launcher, new[] { 7777, 7778, 7779 }, "localhost:5000",
            new RequestedGameSettings(NoJoinSeconds: 180, EmptySeconds: 60));
        roster.PublicIp = "203.0.113.10";
        roster.Reconcile(T0);
        return (roster, launcher);
    }

    static string Token(ServerRoster roster, StartResult r) => roster.Instances.Single(i => i.PublicId == r.Guid).MyGuid;

    static GameRequest Story(string name = "DJ's Algorithm") => new()
    {
        Name = name, MapMode = "Story - The Algorithm", MapCommand = "open Caverns_P", Players = 1,
        BotsTeamA = 5, BotsTeamB = 5, PlayerName = "DJ",
    };

    static GameRequest Versus() => new()
    {
        Name = "DJ's Overgrowth", MapMode = "Incursion - Overgrowth", MapCommand = "open Inc_Stronghold2_P",
        Players = 2, BotsTeamA = 5, BotsTeamB = 3, PlayerName = "DJ",
    };

    [Fact]
    public void StartRequested_launches_on_a_free_port_with_the_requesters_label()
    {
        var (roster, launcher) = Make(Algorithm);

        var result = roster.StartRequested(Story(), "dj", T0);

        Assert.True(result.Ok, result.Error);
        var inst = roster.Instances.Single(i => i.PublicId == result.Guid);
        Assert.Null(inst.Slot);
        Assert.Equal(7778, inst.Port);
        Assert.Equal("dj", inst.KeyLabel);
        Assert.Equal("DJ's Algorithm", inst.Config.InstanceName);
        Assert.Equal("open Caverns_P", inst.Config.ServerStartupCommand);   // no bots on a story map
        Assert.Equal(1, inst.Config.MaxNumPlayers);
        Assert.Contains(launcher.Launched, l => l.Instance == "rb-req-7778" && l.CommandLine.Contains("-rbcoordinatorkey=" + inst.MyGuid));
    }

    [Fact]
    public void StartRequested_appends_bots_for_versus_maps()
    {
        var (roster, _) = Make();
        var result = roster.StartRequested(Versus(), "dj", T0);
        Assert.True(result.Ok);
        Assert.Equal("open Inc_Stronghold2_P?SpawnBotsTeamA=5?SpawnBotsTeamB=3",
            roster.Instances.Single().Config.ServerStartupCommand);
    }

    [Fact]
    public void StartRequested_refuses_when_every_port_is_taken()
    {
        var (roster, _) = Make(Algorithm);
        Assert.True(roster.StartRequested(Story("one"), "dj", T0).Ok);
        Assert.True(roster.StartRequested(Story("two"), "dj", T0).Ok);

        var full = roster.StartRequested(Story("three"), "dj", T0);

        Assert.False(full.Ok);
        Assert.Equal("Server is full (3 of 3 games running). Try again later.", full.Error);
        Assert.Equal(3, roster.Instances.Count);
    }

    [Fact]
    public void StartRequested_never_reuses_a_port_in_use()
    {
        var (roster, _) = Make();
        var a = roster.StartRequested(Story("a"), "dj", T0);
        var b = roster.StartRequested(Story("b"), "dj", T0);
        var c = roster.StartRequested(Story("c"), "dj", T0);
        Assert.Equal(3, roster.Instances.Select(i => i.Port).Distinct().Count());
    }

    [Theory]
    [InlineData("open Caverns_P?Listen")]
    [InlineData("Caverns_P")]
    [InlineData("open ../x")]
    [InlineData("open Caverns_P\n")]
    [InlineData("")]
    public void StartRequested_rejects_unknown_map_commands(string cmd)
    {
        var (roster, _) = Make();
        var req = Story(); req.MapCommand = cmd;
        var r = roster.StartRequested(req, "dj", T0);
        Assert.False(r.Ok);
        Assert.Contains("map", r.Error);
        Assert.Empty(roster.Instances);
    }

    [Theory]
    [InlineData(0, 5, 5)]
    [InlineData(6, 5, 5)]    // story allows 1..5
    [InlineData(2, 6, 0)]    // bots 0..5
    [InlineData(2, 0, -1)]
    public void StartRequested_rejects_bad_player_and_bot_counts(int players, int a, int b)
    {
        var (roster, _) = Make();
        var req = Story(); req.Players = players; req.BotsTeamA = a; req.BotsTeamB = b;
        Assert.False(roster.StartRequested(req, "dj", T0).Ok);
    }

    [Fact]
    public void Versus_allows_up_to_ten_players()
    {
        var (roster, _) = Make();
        var req = Versus(); req.Players = 10;
        Assert.True(roster.StartRequested(req, "dj", T0).Ok);
    }

    [Fact]
    public void Reconcile_drops_a_requested_server_that_never_checked_in()
    {
        var (roster, launcher) = Make();
        var r = roster.StartRequested(Story(), "dj", T0);

        roster.Reconcile(T0.AddSeconds(121));   // StartupGrace is 120 s

        Assert.Empty(roster.Instances);
        Assert.Single(launcher.Launched);
    }

    [Fact]
    public void Reconcile_shuts_a_requested_server_nobody_joined_after_NoJoinSeconds()
    {
        var (roster, launcher) = Make();
        var r = roster.StartRequested(Story(), "dj", T0);
        roster.RecordPoll(Token(roster, r), 0, false, T0.AddSeconds(20));
        roster.RecordPoll(Token(roster, r), 0, false, T0.AddSeconds(199));
        roster.Reconcile(T0.AddSeconds(199));
        Assert.Single(roster.Instances);

        roster.RecordPoll(Token(roster, r), 0, false, T0.AddSeconds(201));
        roster.Reconcile(T0.AddSeconds(201));

        Assert.Empty(roster.Instances);
        Assert.Equal(new[] { 100 }, launcher.Killed);
        Assert.Single(launcher.Launched);
    }

    [Fact]
    public void Reconcile_shuts_a_requested_server_EmptySeconds_after_the_last_player_left()
    {
        var (roster, launcher) = Make();
        var r = roster.StartRequested(Story(), "dj", T0);
        roster.RecordPoll(Token(roster, r), 1, true, T0.AddSeconds(30));
        roster.RecordPoll(Token(roster, r), 1, true, T0.AddSeconds(299));    // last time anyone was there
        for (int t = 300; t <= 359; t += 10)                         // then the server keeps checking in, empty
            roster.RecordPoll(Token(roster, r), 0, true, T0.AddSeconds(t));
        roster.RecordPoll(Token(roster, r), 0, true, T0.AddSeconds(359));
        roster.Reconcile(T0.AddSeconds(359));
        Assert.Single(roster.Instances);

        roster.RecordPoll(Token(roster, r), 0, true, T0.AddSeconds(361));
        roster.Reconcile(T0.AddSeconds(361));

        Assert.Empty(roster.Instances);
        Assert.Equal(new[] { 100 }, launcher.Killed);
    }

    [Fact]
    public void A_requested_server_that_finished_or_was_cancelled_is_not_relaunched()
    {
        var (roster, launcher) = Make();
        var r = roster.StartRequested(Story(), "dj", T0);
        Assert.True(roster.Cancel(r.Guid!));
        roster.Reconcile(T0.AddSeconds(1));
        Assert.Empty(roster.Instances);
        Assert.Single(launcher.Launched);
        Assert.False(roster.Cancel("no-such-guid"));
    }

    [Fact]
    public void BrowserEntries_carry_guid_readiness_and_late_join_rule()
    {
        var (roster, _) = Make(Algorithm);
        var story = roster.StartRequested(Story(), "dj", T0);
        var versus = roster.StartRequested(Versus(), "dj", T0);
        roster.RecordPoll(Token(roster, story), 0, false, T0.AddSeconds(20));

        var entries = roster.BrowserEntries();

        var s = entries.Single(e => e.MyGuid == story.Guid);
        var v = entries.Single(e => e.MyGuid == versus.Guid);
        Assert.True(s.Ready); Assert.False(v.Ready);
        Assert.True(s.AllowLateJoin); Assert.False(v.AllowLateJoin);
        Assert.True(entries.Single(e => e.InstanceName == "Algorithm").AllowLateJoin);
    }

    [Fact]
    public void Browser_entry_id_is_not_the_servers_private_token_and_cancel_uses_the_public_id()
    {
        var (roster, launcher) = Make();
        var r = roster.StartRequested(Story(), "dj", T0);
        string token = Token(roster, r);

        var entry = Assert.Single(roster.BrowserEntries());
        Assert.Equal(r.Guid, entry.MyGuid);
        Assert.NotEqual(token, entry.MyGuid);
        Assert.Contains(launcher.Launched, l => l.CommandLine.Contains("-rbcoordinatorkey=" + token));
        Assert.DoesNotContain(launcher.Launched, l => l.CommandLine.Contains(entry.MyGuid));

        Assert.False(roster.Cancel(token));          // the private token is not a cancel handle
        Assert.True(roster.Cancel(entry.MyGuid));
    }

    [Theory]
    [InlineData("Story - The Algorithm", true)]
    [InlineData("Operations - Attikus and the Thrall Rebellion", true)]
    [InlineData("Incursion - Overgrowth", false)]
    [InlineData("Supercharge - Ziggurat", false)]
    public void IsLateJoinMode_is_story_and_operations_only(string mode, bool expected)
    {
        Assert.Equal(expected, ServerRoster.IsLateJoinMode(mode));
    }
}
