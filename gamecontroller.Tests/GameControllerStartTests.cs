using gamecontroller.Controllers;
using gamecontroller.Models;
using gamecontroller.Services;
using gamecontroller.Singletons;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging.Abstractions;

namespace gamecontroller.Tests;

public class GameControllerStartTests
{
    static readonly DateTime T0 = new(2026, 10, 15, 12, 0, 0, DateTimeKind.Utc);

    static (Game controller, ServerRoster roster) Make()
    {
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(Array.Empty<RosterSlot>(), launcher, new[] { 7777 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        var keys = new StartKeys(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["StartKeys:dj"] = "GOODKEY" }).Build());
        var controller = new Game(NullLogger<Game>.Instance, new LobbySingleton(), roster, keys, new StinkyWordService())
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
        var launcher = new FakeLauncher();
        var roster = new ServerRoster(Array.Empty<RosterSlot>(), launcher, new[] { 7777 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        var keys = new StartKeys(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?>
            { ["StartKeys:a"] = "KEYA", ["StartKeys:b"] = "KEYB" }).Build());
        var c = new Game(NullLogger<Game>.Instance, new LobbySingleton(), roster, keys, new StinkyWordService())
        {
            ControllerContext = new ControllerContext { HttpContext = new DefaultHttpContext() },
        };
        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYA";
        c.Start(Req());
        string id = roster.Instances.Single().PublicId;

        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYB";
        Assert.Equal(404, Status(c.Cancel(id)));
        Assert.Single(roster.Instances.Where(i => !i.Finished));

        c.HttpContext.Request.Headers["X-Start-Key"] = "KEYA";
        Assert.Equal(200, Status(c.Cancel(id)));
    }
}
