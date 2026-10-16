using gamecontroller.Singletons;
using Microsoft.Extensions.Configuration;

namespace gamecontroller.Tests;

// A roster game can give every player a random hero ("RandomHero": true in appsettings.json).
public class RosterRandomTests
{
    static readonly DateTime T0 = new(2026, 10, 15, 12, 0, 0, DateTimeKind.Utc);

    static (ServerRoster, FakeLauncher) MakeTwo()
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(new[]
            {
                new RosterSlot("Incursion vs Bots", "Incursion - Echelon", "open Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5", 1, RandomHero: true),
                new RosterSlot("Plain", "Story - The Algorithm", "open Caverns_P", 2),
            },
            launcher, new[] { 7777, 7778 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        roster.Reconcile(T0);
        return (roster, launcher);
    }

    static string TokenOf(ServerRoster roster, string name) => roster.Instances.Single(i => i.Config.InstanceName == name).MyGuid;

    [Fact]
    public void Random_hero_is_set_for_that_roster_game_only()
    {
        var (roster, _) = MakeTwo();
        Assert.True(roster.ConfigFor(TokenOf(roster, "Incursion vs Bots"))!.RandomHero);
        Assert.False(roster.ConfigFor(TokenOf(roster, "Plain"))!.RandomHero);
    }

    [Fact]
    public void RandomHero_reaches_the_server_through_server_config_not_the_command_line()
    {
        var (roster, launcher) = MakeTwo();
        string token = TokenOf(roster, "Incursion vs Bots");
        Assert.True(roster.ConfigFor(token)!.RandomHero);
        var launched = launcher.Launched.Single(l => l.CommandLine.Contains("-rbcoordinatorkey=" + token));
        Assert.DoesNotContain("-rbrandomhero", launched.CommandLine);
        Assert.All(launcher.Launched, l => Assert.DoesNotContain("rbrandomhero", l.CommandLine));
    }

    [Fact]
    public void Settings_with_and_without_RandomHero_both_read()
    {
        var config = new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?>
        {
            ["Roster:0:Name"] = "The Algorithm",
            ["Roster:0:MapMode"] = "Story - The Algorithm",
            ["Roster:0:ServerStartupCommand"] = "open Caverns_P",
            ["Roster:0:MaxNumPlayers"] = "2",
            ["Roster:1:Name"] = "Incursion vs Bots",
            ["Roster:1:MapMode"] = "Incursion - Echelon",
            ["Roster:1:ServerStartupCommand"] = "open Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5",
            ["Roster:1:MaxNumPlayers"] = "1",
            ["Roster:1:RandomHero"] = "true",
        }).Build();
        var slots = config.GetSection("Roster").Get<List<RosterSlot>>()!;
        Assert.False(slots[0].RandomHero);
        Assert.True(slots[1].RandomHero);
        Assert.Equal(1, slots[1].MaxNumPlayers);
    }
}
