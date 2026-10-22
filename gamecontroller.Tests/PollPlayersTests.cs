using gamecontroller.Controllers;
using gamecontroller.Models;
using gamecontroller.Services;
using gamecontroller.Singletons;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace gamecontroller.Tests;

// Remembers every log line, so tests can check what was (and was not) written.
class ListLogger<T> : ILogger<T>
{
    public List<string> Lines = new();
    public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
    public bool IsEnabled(LogLevel logLevel) => true;
    public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter) =>
        Lines.Add(formatter(state, exception));
}

public class PollPlayersTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "gcpoll-" + Guid.NewGuid().ToString("N"));
    public PollPlayersTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { } }

    static readonly DateTime T0 = new(2026, 10, 22, 18, 0, 0, DateTimeKind.Utc);

    (Game controller, ServerRoster roster, SeenLog seen, ListLogger<Game> log, string token) Make()
    {
        var roster = new ServerRoster(Array.Empty<RosterSlot>(), new FakeLauncher(), new[] { 7777 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        roster.StartRequested(new GameRequest { Name = "DJ's Algorithm", MapMode = "Story - The Algorithm", MapCommand = "open Caverns_P", Players = 2 }, "dj", T0);
        var users = new UserList(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["UsersFile"] = Path.Combine(_dir, "reborn-users.json") }).Build());
        var seen = new SeenLog(Path.Combine(_dir, "reborn-users-seen.json"), () => T0);
        var log = new ListLogger<Game>();
        var controller = new Game(log, roster, users, new StinkyWordService(), seen)
        {
            ControllerContext = new ControllerContext { HttpContext = new DefaultHttpContext() },
        };
        string token = roster.Instances.Single().MyGuid;
        controller.HttpContext.Request.Headers["X-Server-Token"] = token;
        return (controller, roster, seen, log, token);
    }

    [Fact]
    public void Poll_players_are_stored_on_the_game()
    {
        var (c, roster, seen, _, _) = Make();

        c.ServerPoll(new ServerPollBody
        {
            ConnectedPlayers = 2, HumansHaveStarted = true,
            Players = new() { new("Sintax", "Oscar Mike", 0), new("Friend", "", -1) },
        });

        var game = roster.Instances.Single();
        Assert.Equal(new[] { new PolledPlayer("Sintax", "Oscar Mike", 0), new PolledPlayer("Friend", "", -1) }, game.Players);
        Assert.Equal(2, game.CurrentNumPlayers);
        Assert.Equal(T0, seen.LastSeen("Sintax"));
        Assert.Equal(T0, seen.LastSeen("Friend"));
    }

    [Fact]
    public void Old_servers_without_players_still_work()
    {
        var (c, roster, _, _, _) = Make();

        c.ServerPoll(new ServerPollBody { ConnectedPlayers = 1, HumansHaveStarted = false });

        var game = roster.Instances.Single();
        Assert.Empty(game.Players);
        Assert.Equal(1, game.CurrentNumPlayers);
        Assert.True(game.HasCheckedIn);
    }

    [Fact]
    public void Poll_players_are_cleaned_and_capped()
    {
        var (_, roster, _, _, token) = Make();
        var players = new List<PolledPlayer> { new("Bad\"Name<script>", new string('h', 200), 99), new("x", "Oscar\nMikeé", -7) };
        for (int n = 0; n < 48; n++) players.Add(new("p" + n, "Rath", 1));

        Assert.True(roster.RecordPoll(token, 5, true, players, T0));

        var stored = roster.Instances.Single().Players;
        Assert.Equal(32, stored.Count);
        Assert.Equal(new PolledPlayer("BadNamescript", new string('h', 40), 15), stored[0]);
        Assert.Equal(new PolledPlayer("x", "OscarMike", -1), stored[1]);
        Assert.Equal("p29", stored[31].Name);
    }

    [Fact]
    public void Long_names_are_cut_to_24_characters()
    {
        var (_, roster, _, _, token) = Make();
        roster.RecordPoll(token, 1, false, new[] { new PolledPlayer(new string('a', 30), null!, 0) }, T0);
        Assert.Equal(new PolledPlayer(new string('a', 24), "", 0), roster.Instances.Single().Players.Single());
    }

    [Fact]
    public void Players_are_logged_only_when_they_change_and_raw_text_never()
    {
        var (c, _, _, log, _) = Make();
        ServerPollBody Body(params PolledPlayer[] p) => new() { ConnectedPlayers = p.Length, Players = p.ToList() };

        c.ServerPoll(Body(new PolledPlayer("Sintax<b>", "Oscar Mike", 0)));
        c.ServerPoll(Body(new PolledPlayer("Sintax<b>", "Oscar Mike", 0)));
        c.ServerPoll(Body(new PolledPlayer("Sintax", "Oscar Mike", 0), new PolledPlayer("Friend", "", -1)));
        c.ServerPoll(Body());

        var lines = log.Lines.Where(l => l.StartsWith("Players in")).ToList();
        Assert.Equal(new[]
        {
            "Players in \"DJ's Algorithm\": Sintaxb (Oscar Mike, team 0)",
            "Players in \"DJ's Algorithm\": Sintax (Oscar Mike, team 0), Friend (no hero yet, team -1)",
            "Players in \"DJ's Algorithm\": none",
        }, lines);
        Assert.DoesNotContain(log.Lines, l => l.Contains('<'));
    }

    [Fact]
    public void Poll_with_an_unknown_token_records_nothing()
    {
        var (c, _, seen, log, _) = Make();
        c.HttpContext.Request.Headers["X-Server-Token"] = "nope";
        c.ServerPoll(new ServerPollBody { ConnectedPlayers = 1, Players = new() { new("Sintax", "Rath", 0) } });
        Assert.Null(seen.LastSeen("Sintax"));
        Assert.Empty(log.Lines);
    }
}
