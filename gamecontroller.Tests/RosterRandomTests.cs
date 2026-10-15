using gamecontroller.Singletons;
using Microsoft.Extensions.Configuration;

namespace gamecontroller.Tests;

// A roster game can give every player a random hero ("RandomHero": true in appsettings.json).
public class RosterRandomTests
{
    static readonly DateTime T0 = new(2026, 10, 15, 12, 0, 0, DateTimeKind.Utc);

    [Fact]
    public void Random_hero_is_asked_for_on_that_game_servers_command_line_only()
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(new[]
            {
                new RosterSlot("Incursion vs Bots", "Incursion - Echelon", "open Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5", 1, RandomHero: true),
                new RosterSlot("Plain", "Story - The Algorithm", "open Caverns_P", 2),
            },
            launcher, new[] { 7777, 7778 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        roster.Reconcile(T0);
        Assert.EndsWith(" -rbrandomhero", launcher.Launched[0].CommandLine);
        Assert.DoesNotContain("rbrandomhero", launcher.Launched[1].CommandLine);
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
