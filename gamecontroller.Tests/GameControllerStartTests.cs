using gamecontroller.Controllers;
using gamecontroller.Models;
using gamecontroller.Services;
using gamecontroller.Singletons;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging.Abstractions;
using System.Text.Json;

namespace gamecontroller.Tests;

public class GameControllerStartTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "gcstart-" + Guid.NewGuid().ToString("N"));
    public GameControllerStartTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { } }

    static readonly DateTime T0 = new(2026, 10, 15, 12, 0, 0, DateTimeKind.Utc);

    // A user list file with the given (name, key, canCreateGames) users.
    UserList Users(params (string name, string key, bool create)[] users)
    {
        string path = Path.Combine(_dir, "reborn-users.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new
        {
            RequireKeys = true,
            Users = users.Select(u => new { Name = u.name, Key = u.key, CanCreateGames = u.create, ConfirmName = false, Added = "2026-10-21T18:00:00Z" }),
        }));
        return new UserList(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["UsersFile"] = path }).Build());
    }

    (Game controller, ServerRoster roster) Make() => Make(Users(("dj", "GOODKEY", true)));

    (Game controller, ServerRoster roster) Make(UserList keys)
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(Array.Empty<RosterSlot>(), launcher, new[] { 7777 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        var controller = new Game(NullLogger<Game>.Instance, roster, keys, new StinkyWordService())
        {
            ControllerContext = new ControllerContext { HttpContext = new DefaultHttpContext() },
        };
        return (controller, roster);
    }

    static GameRequest Req(string name = "DJ's Algorithm") => new()
    {
        Name = name, MapMode = "Story - The Algorithm", MapCommand = "open Caverns_P", Players = 1, PlayerName = "DJ",
    };

    static int Status(IActionResult r) => (r as ObjectResult)?.StatusCode ?? (r as StatusCodeResult)?.StatusCode ?? 200;

    [Fact]
    public void Start_with_a_good_key_launches_and_answers_the_guid()
    {
        var (c, roster) = Make();
        c.HttpContext.Request.Headers["X-Start-Key"] = "GOODKEY";

        var result = c.Start(Req());

        Assert.Equal(200, Status(result));
        var body = Assert.IsType<OkObjectResult>(result).Value!;
        string guid = (string)body.GetType().GetProperty("guid")!.GetValue(body)!;
        Assert.Equal(guid, roster.Instances.Single().PublicId);
        Assert.Equal("dj", roster.Instances.Single().KeyLabel);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("WRONGKEY")]
    public void Start_without_a_valid_key_is_401_and_launches_nothing(string? key)
    {
        var (c, roster) = Make();
        if (key != null) c.HttpContext.Request.Headers["X-Start-Key"] = key;
        var result = c.Start(Req());
        Assert.Equal(401, Status(result));
        Assert.Empty(roster.Instances);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("12345678901234567890123456789012345678901")]   // 41 chars
    public void Start_rejects_bad_names(string name)
    {
        var (c, roster) = Make();
        c.HttpContext.Request.Headers["X-Start-Key"] = "GOODKEY";
        Assert.Equal(400, Status(c.Start(Req(name))));
        Assert.Empty(roster.Instances);
    }

    [Fact]
    public void Start_when_full_is_409()
    {
        var (c, _) = Make();
        c.HttpContext.Request.Headers["X-Start-Key"] = "GOODKEY";
        Assert.Equal(200, Status(c.Start(Req("one"))));
        var full = c.Start(Req("two"));
        Assert.Equal(409, Status(full));
    }

    [Fact]
    public void Cancel_needs_a_key_and_retires_the_game()
    {
        var (c, roster) = Make();
        c.HttpContext.Request.Headers["X-Start-Key"] = "GOODKEY";
        c.Start(Req());
        string guid = roster.Instances.Single().PublicId;

        Assert.Equal(200, Status(c.Cancel(guid)));
        roster.Reconcile(T0);
        Assert.Empty(roster.Instances);
        Assert.Equal(404, Status(c.Cancel(guid)));

        c.HttpContext.Request.Headers["X-Start-Key"] = "WRONGKEY";
        Assert.Equal(401, Status(c.Cancel(guid)));
    }

    [Fact]
    public void Only_the_key_that_started_a_game_can_cancel_it()
    {
        var (c, roster) = Make(Users(("a", "KEYA", true), ("b", "KEYB", true)));
        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYA";
        c.Start(Req());
        string id = roster.Instances.Single().PublicId;

        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYB";
        Assert.Equal(404, Status(c.Cancel(id)));
        Assert.Single(roster.Instances.Where(i => !i.Finished));

        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYA";
        Assert.Equal(200, Status(c.Cancel(id)));
    }

    [Fact]
    public void Start_refuses_a_user_without_game_creation_with_403()
    {
        var (c, roster) = Make(Users(("guest", "GUESTKEY", false)));
        c.HttpContext.Request.Headers["X-Start-Key"] = "GUESTKEY";

        var result = c.Start(Req());

        Assert.Equal(403, Status(result));
        Assert.Empty(roster.Instances);
        var body = Assert.IsType<ObjectResult>(result).Value!;
        Assert.Equal("Your account can't start games on this server.", (string)body.GetType().GetProperty("message")!.GetValue(body)!);
    }

    [Fact]
    public void Cancel_refuses_a_user_without_game_creation_with_403()
    {
        var (c, _) = Make(Users(("guest", "GUESTKEY", false)));
        c.HttpContext.Request.Headers["X-Start-Key"] = "GUESTKEY";
        Assert.Equal(403, Status(c.Cancel("any-game")));
    }
}
