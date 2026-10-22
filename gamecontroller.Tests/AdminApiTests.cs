using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;
using gamecontroller.Controllers;
using gamecontroller.Models;
using gamecontroller.Services;
using gamecontroller.Singletons;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.Abstractions;
using Microsoft.AspNetCore.Mvc.Filters;
using Microsoft.AspNetCore.Mvc.Infrastructure;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.Configuration;

namespace gamecontroller.Tests;

public class AdminApiTests : IDisposable
{
    const string DjKey = "ABCDEFGHJKMNPQRSTVWXYZ23";
    const string GuestKey = "ZYXWVTSRQPNMKJHGFEDCBA32";
    const string KazKey = "KAZKAZKAZKAZKAZKAZKAZKAZ";
    const string NewKey = "2345678923456789ABCDEFGH";

    static readonly DateTime T0 = new(2026, 10, 22, 18, 0, 0, DateTimeKind.Utc);

    readonly string _dir = Path.Combine(Path.GetTempPath(), "gcadmin-" + Guid.NewGuid().ToString("N"));
    string UsersPath => Path.Combine(_dir, "reborn-users.json");
    string SettingsPath => Path.Combine(_dir, "appsettings.json");
    string TokenPath => Path.Combine(_dir, "matchmaking", AdminToken.FileName);

    public AdminApiTests() => Directory.CreateDirectory(_dir);
    public void Dispose() { try { Directory.Delete(_dir, true); } catch { } }

    static string UsersJson(bool requireKeys, params (string name, string key, bool create, bool confirm)[] users) =>
        JsonSerializer.Serialize(new
        {
            RequireKeys = requireKeys,
            Users = users.Select(u => new { Name = u.name, Key = u.key, CanCreateGames = u.create, ConfirmName = u.confirm, Added = "2026-10-21T18:00:00Z" }),
        });

    const string Settings = """
        {
          // the service's own settings: everything but GamePorts must survive a ports change
          "Logging": { "LogLevel": { "Default": "Information" } },
          "Urls": "http://0.0.0.0:5000",
          "Win64": null,
          "PublicIp": null,
          "GamePorts": [ 7777, 7778, 7779 ],
          "Roster": [ { "Name": "The Algorithm", "MapMode": "Story - The Algorithm", "ServerStartupCommand": "open Caverns_P", "MaxNumPlayers": 2 } ],
          "RequestedGame": { "NoJoinSeconds": 180, "EmptySeconds": 60 }
        }
        """;

    sealed record Rig(Admin Controller, ServerRoster Roster, SeenLog Seen, ListLogger<Admin> Log);

    Rig Make(string? usersJson = null)
    {
        File.WriteAllText(UsersPath, usersJson ?? UsersJson(true, ("dj", DjKey, true, false), ("guest", GuestKey, false, true)));
        if (!File.Exists(SettingsPath)) File.WriteAllText(SettingsPath, Settings);
        var config = new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?>
        {
            ["UsersFile"] = UsersPath,
            ["Win64"] = _dir,
        }).Build();
        var roster = new ServerRoster(Array.Empty<RosterSlot>(), new FakeLauncher(), new[] { 7777, 7778 }, "localhost:5000") { PublicIp = "203.0.113.10" };
        var seen = new SeenLog(Path.Combine(_dir, SeenLog.FileName), () => T0);
        var log = new ListLogger<Admin>();
        var controller = new Admin(roster, new UserList(config) { RecheckEvery = TimeSpan.Zero }, new UserListWriter(UsersPath),
            new SettingsWriter(SettingsPath), seen, config, new ServiceStarted(T0), log)
        {
            ControllerContext = new ControllerContext { HttpContext = new DefaultHttpContext() },
        };
        return new Rig(controller, roster, seen, log);
    }

    static int Status(IActionResult r) => (r as ObjectResult)?.StatusCode ?? (r as StatusCodeResult)?.StatusCode ?? 200;

    static T Body<T>(IActionResult r) => Assert.IsType<T>(Assert.IsAssignableFrom<ObjectResult>(r).Value);

    static string Message(IActionResult r)
    {
        object body = Assert.IsAssignableFrom<ObjectResult>(r).Value!;
        return (string)body.GetType().GetProperty("message")!.GetValue(body)!;
    }

    // What the API would send, as JSON text (controllers use camelCase).
    static string Wire(IActionResult r) =>
        JsonSerializer.Serialize(Assert.IsAssignableFrom<ObjectResult>(r).Value, JsonSerializerOptions.Web);

    AdminUsers ListUsers(Admin c) => Body<AdminUsers>(c.GetUsers());

    // --- the guard ----------------------------------------------------------------------------

    static AuthorizationFilterContext Request(string? ip, string? token)
    {
        var http = new DefaultHttpContext();
        if (ip != null) http.Connection.RemoteIpAddress = IPAddress.Parse(ip);
        if (token != null) http.Request.Headers["X-Admin-Token"] = token;
        return new AuthorizationFilterContext(new ActionContext(http, new RouteData(), new ActionDescriptor()), new List<IFilterMetadata>());
    }

    static bool Refused(AdminGuard guard, string? ip, string? token)
    {
        var ctx = Request(ip, token);
        guard.OnAuthorization(ctx);
        if (ctx.Result == null) return false;
        // 403 and nothing else: no body that could say why. (A StatusCodeResult would be turned into a
        // problem-details body by [ApiController], so the refusal must not be one.)
        Assert.IsNotAssignableFrom<IClientErrorActionResult>(ctx.Result);
        ctx.HttpContext.Response.Body = new MemoryStream();
        ctx.Result.ExecuteResultAsync(ctx).GetAwaiter().GetResult();
        Assert.Equal(403, ctx.HttpContext.Response.StatusCode);
        Assert.Equal(0, ctx.HttpContext.Response.Body.Length);
        return true;
    }

    [Fact]
    public void The_admin_controller_is_guarded()
    {
        var filter = typeof(Admin).GetCustomAttributes(typeof(ServiceFilterAttribute), true).Cast<ServiceFilterAttribute>().Single();
        Assert.Equal(typeof(AdminGuard), filter.ServiceType);
        var route = typeof(Admin).GetCustomAttributes(typeof(RouteAttribute), true).Cast<RouteAttribute>().Single();
        Assert.Equal("api/admin", route.Template);
    }

    [Fact]
    public void Requests_from_other_machines_are_refused()
    {
        var token = AdminToken.LoadOrCreate(TokenPath);
        string good = File.ReadAllText(TokenPath).Trim();
        var guard = new AdminGuard(token);

        Assert.True(Refused(guard, "203.0.113.10", good));
        Assert.True(Refused(guard, "192.168.1.20", good));
        Assert.True(Refused(guard, "2001:db8::1", good));
        Assert.True(Refused(guard, null, good));
        Assert.False(Refused(guard, "127.0.0.1", good));
        Assert.False(Refused(guard, "::1", good));
        Assert.False(Refused(guard, "::ffff:127.0.0.1", good));
    }

    [Fact]
    public void Requests_without_the_token_are_refused()
    {
        var token = AdminToken.LoadOrCreate(TokenPath);
        string good = File.ReadAllText(TokenPath).Trim();
        var guard = new AdminGuard(token);

        Assert.True(Refused(guard, "127.0.0.1", null));
        Assert.True(Refused(guard, "127.0.0.1", ""));
        Assert.True(Refused(guard, "127.0.0.1", "nope"));
        Assert.True(Refused(guard, "127.0.0.1", good.ToLowerInvariant()));
        Assert.True(Refused(guard, "127.0.0.1", good + "X"));
        Assert.True(Refused(guard, "127.0.0.1", good[..^1]));
        Assert.False(Refused(guard, "127.0.0.1", good));
    }

    [Fact]
    public void Token_file_is_created_once_and_kept()
    {
        Assert.False(File.Exists(TokenPath));
        var first = AdminToken.LoadOrCreate(TokenPath);
        string text = File.ReadAllText(TokenPath).Trim();
        Assert.Equal(32, text.Length);
        Assert.All(text, ch => Assert.Contains(ch, UserListWriter.KeyAlphabet));
        Assert.True(first.Matches(text));

        var second = AdminToken.LoadOrCreate(TokenPath);
        Assert.Equal(text, File.ReadAllText(TokenPath).Trim());
        Assert.True(second.Matches(text));
        Assert.False(second.Matches(null));
        Assert.False(second.Matches(""));

        // Another server's token file makes a different token.
        string otherPath = Path.Combine(_dir, "other", AdminToken.FileName);
        AdminToken.LoadOrCreate(otherPath);
        Assert.NotEqual(text, File.ReadAllText(otherPath).Trim());
        Assert.False(second.Matches(File.ReadAllText(otherPath).Trim()));
    }

    // --- status and games ---------------------------------------------------------------------

    [Fact]
    public void Status_reports_version_ports_and_ip()
    {
        File.WriteAllText(Path.Combine(_dir, "reborn-deployed.txt"), "  2026-10-22 bab2c4e  \r\n");
        var rig = Make();
        rig.Roster.StartRequested(new GameRequest { Name = "One", MapMode = "Story - The Algorithm", MapCommand = "open Caverns_P", Players = 2 }, "dj", T0);

        var status = Body<AdminStatus>(rig.Controller.GetStatus());

        Assert.Equal("2026-10-22 bab2c4e", status.Version);
        Assert.Equal("203.0.113.10", status.PublicIp);
        Assert.Equal(new[] { 7777, 7778, 7779 }, status.GamePorts);
        Assert.Equal(1, status.GamesRunning);
        Assert.True(status.RequireKeys);
        Assert.Equal(T0, status.MatchmakingStartedUtc);

        string wire = Wire(rig.Controller.GetStatus());
        foreach (string field in new[] { "version", "publicIp", "gamePorts", "gamesRunning", "requireKeys", "matchmakingStartedUtc" })
            Assert.Contains("\"" + field + "\"", wire);

        // No deployed-version file: an empty version, not an error.
        File.Delete(Path.Combine(_dir, "reborn-deployed.txt"));
        Assert.Equal("", Body<AdminStatus>(rig.Controller.GetStatus()).Version);
    }

    [Fact]
    public void Games_list_players_and_heroes()
    {
        var rig = Make();
        rig.Roster.StartRequested(new GameRequest
        {
            Name = "DJ's Incursion", MapMode = "Incursion - Overgrowth", MapCommand = "open Overgrowth_P", Players = 2, DraftStyle = DraftStyles.QuickSelect,
        }, "dj", T0);
        var game = rig.Roster.Instances.Single();
        rig.Roster.RecordPoll(game.MyGuid, 2, true, new[] { new PolledPlayer("dj", "Rath", 0), new PolledPlayer("Kaz", "", -1) }, T0.AddSeconds(5));

        var games = Body<List<AdminGame>>(rig.Controller.GetGames());

        var g = Assert.Single(games);
        Assert.Equal(game.PublicId, g.PublicId);
        Assert.Equal("DJ's Incursion", g.Name);
        Assert.Equal("Incursion - Overgrowth", g.MapMode);
        Assert.Equal("", g.DraftStyle);
        Assert.False(g.RandomHero);
        Assert.Equal(7777, g.Port);
        Assert.Equal(T0, g.StartedUtc);
        Assert.True(g.MatchStarted);
        Assert.False(g.IsRoster);
        Assert.Equal("dj", g.StartedBy);
        Assert.Equal(new[] { new AdminPlayer("dj", "Rath", 0), new AdminPlayer("Kaz", "", -1) }, g.Players);

        // The server's private token never leaves through this list.
        Assert.DoesNotContain(game.MyGuid, Wire(rig.Controller.GetGames()));
    }

    // --- users --------------------------------------------------------------------------------

    [Fact]
    public void Users_list_never_contains_keys()
    {
        var rig = Make();
        rig.Seen.Saw(new[] { "DJ" });

        var result = rig.Controller.GetUsers();
        var users = Body<AdminUsers>(result);

        Assert.True(users.RequireKeys);
        Assert.Equal(new[] { "dj", "guest" }, users.Users.Select(u => u.Name));
        var dj = users.Users[0];
        Assert.True(dj.CanCreateGames);
        Assert.False(dj.ConfirmName);
        Assert.Equal(new DateTime(2026, 10, 21, 18, 0, 0, DateTimeKind.Utc), dj.Added);
        Assert.Equal(T0, dj.LastSeen);
        Assert.Null(users.Users[1].LastSeen);
        Assert.True(users.Users[1].ConfirmName);

        string wire = Wire(result);
        Assert.DoesNotContain(DjKey, wire);
        Assert.DoesNotContain(GuestKey, wire);
        Assert.DoesNotContain("\"key\"", wire, StringComparison.OrdinalIgnoreCase);

        // Only the one-user key call gives a key.
        Assert.Equal(DjKey, Body<AdminKeyReply>(rig.Controller.GetKey("DJ")).Key);
        Assert.Equal(404, Status(rig.Controller.GetKey("nobody")));
    }

    [Fact]
    public void Add_rename_flag_replace_key_and_revoke_a_user()
    {
        var rig = Make();
        var c = rig.Controller;

        var added = c.AddUser(new AdminNewUser { Name = "Kaz", Key = KazKey, CanCreateGames = false });
        Assert.Equal(201, Status(added));
        Assert.Equal("Kaz", Body<AdminUser>(added).Name);
        Assert.Contains("Kaz", ListUsers(c).Users.Select(u => u.Name));
        Assert.Equal(KazKey, Body<AdminKeyReply>(c.GetKey("kaz")).Key);

        // The file stays in the shape every reader accepts, and the server's own reader sees the change.
        string text = File.ReadAllText(UsersPath);
        Assert.EndsWith("\n", text);
        Assert.Contains("\n  \"Users\": [", text.Replace("\r\n", "\n"));
        var kaz = JsonNode.Parse(text)!["Users"]!.AsArray().Single(u => (string)u!["Name"]! == "Kaz")!;
        Assert.Equal(KazKey, (string)kaz["Key"]!);
        Assert.False((bool)kaz["CanCreateGames"]!);
        Assert.False((bool)kaz["ConfirmName"]!);
        Assert.Matches(@"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$", (string)kaz["Added"]!);
        var reader = new UserList(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["UsersFile"] = UsersPath }).Build());
        Assert.Equal("Kaz", reader.ByKey(KazKey)?.Name);

        // Rename: the player must now join with that name, so ConfirmName goes off.
        var renamed = c.UpdateUser("guest", new AdminUserChange { NewName = "Mike" });
        Assert.Equal(200, Status(renamed));
        var mike = Body<AdminUser>(renamed);
        Assert.Equal("Mike", mike.Name);
        Assert.False(mike.ConfirmName);
        Assert.DoesNotContain("guest", ListUsers(c).Users.Select(u => u.Name));
        Assert.Equal(GuestKey, Body<AdminKeyReply>(c.GetKey("Mike")).Key);

        // Flag on and off.
        Assert.Equal(200, Status(c.UpdateUser("mike", new AdminUserChange { CanCreateGames = true })));
        Assert.True(ListUsers(c).Users.Single(u => u.Name == "Mike").CanCreateGames);
        Assert.Equal(200, Status(c.UpdateUser("Mike", new AdminUserChange { CanCreateGames = false })));
        Assert.False(ListUsers(c).Users.Single(u => u.Name == "Mike").CanCreateGames);

        // A new key replaces the old one.
        Assert.Equal(204, Status(c.ReplaceKey("Kaz", new AdminKey { Key = NewKey })));
        Assert.Equal(NewKey, Body<AdminKeyReply>(c.GetKey("Kaz")).Key);

        // Revoke.
        Assert.Equal(204, Status(c.RemoveUser("Kaz")));
        Assert.DoesNotContain("Kaz", ListUsers(c).Users.Select(u => u.Name));
        Assert.Equal(404, Status(c.GetKey("Kaz")));
        Assert.Equal(404, Status(c.RemoveUser("Kaz")));
        Assert.Equal(404, Status(c.UpdateUser("Kaz", new AdminUserChange { CanCreateGames = true })));
        Assert.Equal(404, Status(c.ReplaceKey("Kaz", new AdminKey { Key = KazKey })));

        // Require keys on and off.
        Assert.Equal(200, Status(c.SetRequireKeys(new AdminRequireKeys { On = false })));
        Assert.False(ListUsers(c).RequireKeys);
        Assert.False((bool)JsonNode.Parse(File.ReadAllText(UsersPath))!["RequireKeys"]!);
        Assert.Equal(200, Status(c.SetRequireKeys(new AdminRequireKeys { On = true })));
        Assert.True(ListUsers(c).RequireKeys);
        Assert.Equal(400, Status(c.SetRequireKeys(new AdminRequireKeys())));

        // Names went into the log; no key ever did.
        Assert.Contains(rig.Log.Lines, l => l.Contains("Kaz"));
        foreach (string key in new[] { DjKey, GuestKey, KazKey, NewKey })
            Assert.DoesNotContain(rig.Log.Lines, l => l.Contains(key));
    }

    [Fact]
    public void Bad_names_and_keys_are_refused_with_a_message()
    {
        var rig = Make();
        var c = rig.Controller;
        string before = File.ReadAllText(UsersPath);

        foreach (string? name in new[] { null, "", "has space", "dj's", "ünï", "1234567890123456789012345", "name\n" })
        {
            var r = c.AddUser(new AdminNewUser { Name = name, Key = KazKey });
            Assert.Equal(400, Status(r));
            Assert.Equal(UserListWriter.BadName, Message(r));
        }
        foreach (string? key in new[] { null, "", "SHORT", KazKey.ToLowerInvariant(), "OOOOOOOOOOOOOOOOOOOOOOOO", KazKey + "K", "11111111111111111111111I" })
        {
            var r = c.AddUser(new AdminNewUser { Name = "Kaz", Key = key });
            Assert.Equal(400, Status(r));
            Assert.Equal(UserListWriter.BadKey, Message(r));
        }

        var taken = c.AddUser(new AdminNewUser { Name = "DJ", Key = KazKey });
        Assert.Equal(409, Status(taken));
        Assert.Equal("That name is taken.", Message(taken));

        var keyTaken = c.AddUser(new AdminNewUser { Name = "Kaz", Key = GuestKey });
        Assert.Equal(409, Status(keyTaken));
        Assert.Equal("That key belongs to someone else.", Message(keyTaken));

        Assert.Equal(400, Status(c.UpdateUser("guest", new AdminUserChange { NewName = "bad name" })));
        Assert.Equal("That name is taken.", Message(c.UpdateUser("guest", new AdminUserChange { NewName = "DJ" })));
        Assert.Equal(400, Status(c.UpdateUser("guest", new AdminUserChange())));
        Assert.Equal(UserListWriter.BadKey, Message(c.ReplaceKey("guest", new AdminKey { Key = "nope" })));
        Assert.Equal("That key belongs to someone else.", Message(c.ReplaceKey("guest", new AdminKey { Key = DjKey })));
        Assert.Equal(400, Status(c.AddUser(null)));

        // Nothing was written by any of these.
        Assert.Equal(before, File.ReadAllText(UsersPath));
        Assert.DoesNotContain(rig.Log.Lines, l => l.Contains(GuestKey) || l.Contains(DjKey) || l.Contains(KazKey));
    }

    [Fact]
    public void The_last_game_creator_cannot_be_removed_or_turned_off()
    {
        var rig = Make();
        var c = rig.Controller;
        const string Lockout = "At least one user must be able to start games.";

        var remove = c.RemoveUser("dj");
        Assert.Equal(409, Status(remove));
        Assert.Equal(Lockout, Message(remove));
        var off = c.UpdateUser("DJ", new AdminUserChange { CanCreateGames = false });
        Assert.Equal(409, Status(off));
        Assert.Equal(Lockout, Message(off));
        // A rename alongside the refused change is not half-applied.
        Assert.Equal(409, Status(c.UpdateUser("dj", new AdminUserChange { NewName = "Sintax", CanCreateGames = false })));
        Assert.Contains("dj", ListUsers(c).Users.Select(u => u.Name));
        Assert.True(ListUsers(c).Users.Single(u => u.Name == "dj").CanCreateGames);

        // Others can still be removed, and once someone else can start games, dj can go.
        Assert.Equal(204, Status(c.RemoveUser("guest")));
        Assert.Equal(201, Status(c.AddUser(new AdminNewUser { Name = "Kaz", Key = KazKey, CanCreateGames = true })));
        Assert.Equal(200, Status(c.UpdateUser("dj", new AdminUserChange { CanCreateGames = false })));
        Assert.Equal(409, Status(c.RemoveUser("Kaz")));
        Assert.Equal(200, Status(c.UpdateUser("dj", new AdminUserChange { CanCreateGames = true })));
        Assert.Equal(204, Status(c.RemoveUser("dj")));
    }

    [Theory]
    [InlineData("{ this is not json")]
    [InlineData("{\"RequireKeys\": true, \"Users\": [ { \"Name\": \"dj\", \"Key\": \"ABCDEFGHJKMNPQRSTVWXYZ23\", \"CanCreateGames\": \"yes\" } ]}")]
    [InlineData("{\"RequireKeys\": \"on\", \"Users\": []}")]
    [InlineData("{\"Users\": [ { \"Name\": \"\", \"Key\": \"ABCDEFGHJKMNPQRSTVWXYZ23\" } ]}")]
    [InlineData("[]")]
    public void Damaged_user_file_is_left_alone(string damaged)
    {
        var rig = Make(damaged);
        var c = rig.Controller;
        const string Damaged = "The user list file is damaged; fix or restore it first.";

        foreach (IActionResult r in new[]
        {
            c.AddUser(new AdminNewUser { Name = "Kaz", Key = KazKey }),
            c.UpdateUser("dj", new AdminUserChange { CanCreateGames = false }),
            c.ReplaceKey("dj", new AdminKey { Key = KazKey }),
            c.RemoveUser("dj"),
            c.SetRequireKeys(new AdminRequireKeys { On = false }),
            c.GetUsers(),
            c.GetKey("dj"),
        })
        {
            Assert.Equal(409, Status(r));
            Assert.Equal(Damaged, Message(r));
        }
        Assert.Equal(damaged, File.ReadAllText(UsersPath));
        Assert.Empty(Directory.GetFiles(_dir, "*.tmp"));
        // The status page still answers.
        Assert.Equal(200, Status(c.GetStatus()));
    }

    [Fact]
    public void A_missing_user_file_starts_a_new_one()
    {
        var rig = Make();
        File.Delete(UsersPath);
        var users = ListUsers(rig.Controller);
        Assert.True(users.RequireKeys);
        Assert.Empty(users.Users);

        Assert.Equal(201, Status(rig.Controller.AddUser(new AdminNewUser { Name = "dj", Key = DjKey, CanCreateGames = true })));
        Assert.True(File.Exists(UsersPath));
        Assert.Equal("dj", Assert.Single(ListUsers(rig.Controller).Users).Name);
    }

    [Fact]
    public void Concurrent_writes_leave_a_valid_file()
    {
        File.WriteAllText(UsersPath, UsersJson(true));
        // A writer per request, as the service would have per caller: the lock must be process-wide.
        var results = new int[20];
        Parallel.For(0, 20, new ParallelOptions { MaxDegreeOfParallelism = 20 }, i =>
        {
            string key = new string(UserListWriter.KeyAlphabet[i], 24);
            var refusal = new UserListWriter(UsersPath).Add("user" + i, key, i == 0, DateTime.UtcNow, out _);
            results[i] = refusal?.Status ?? 201;
        });

        Assert.All(results, s => Assert.Equal(201, s));
        var reader = new UserListWriter(UsersPath);
        Assert.Null(reader.Read(out UserFileView? view));
        Assert.Equal(20, view!.Users.Count);
        Assert.Equal(20, view.Users.Select(u => u.Name).Distinct().Count());
        Assert.Empty(Directory.GetFiles(_dir, "*.tmp"));
    }

    // --- ports --------------------------------------------------------------------------------

    [Fact]
    public void Ports_change_is_saved_for_the_next_restart()
    {
        var rig = Make();
        var c = rig.Controller;
        rig.Roster.StartRequested(new GameRequest { Name = "Busy", MapMode = "Story - The Algorithm", MapCommand = "open Caverns_P", Players = 2 }, "dj", T0);
        var running = rig.Roster.Instances.Single();
        JsonNode beforeOthers = WithoutPorts(SettingsPath);

        var saved = c.SetPorts(new AdminPorts { Ports = new List<int> { 7777, 7780 } });

        Assert.Equal(200, Status(saved));
        Assert.Contains("\"restartNeeded\":true", Wire(saved));
        var node = JsonNode.Parse(File.ReadAllText(SettingsPath), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip })!;
        Assert.Equal(new[] { 7777, 7780 }, node["GamePorts"]!.AsArray().Select(p => (int)p!));
        Assert.True(JsonNode.DeepEquals(beforeOthers, WithoutPorts(SettingsPath)));
        Assert.Contains("// the service's own settings", File.ReadAllText(SettingsPath));
        Assert.Equal(new[] { 7777, 7780 }, Body<AdminStatus>(c.GetStatus()).GamePorts);
        // The game on the port that went away keeps running.
        Assert.Same(running, rig.Roster.Instances.Single());
        Assert.False(running.Finished);
        Assert.Equal(7777, running.Port);
        Assert.Empty(Directory.GetFiles(_dir, "*.tmp"));

        // Bad lists are refused and change nothing.
        string text = File.ReadAllText(SettingsPath);
        foreach (List<int>? bad in new List<int>?[]
        {
            null, new(), Enumerable.Range(8000, 17).ToList(), new() { 80 }, new() { 1023 }, new() { 65536 }, new() { 7777, 7777 }, new() { 0 },
        })
        {
            var r = c.SetPorts(new AdminPorts { Ports = bad });
            Assert.Equal(400, Status(r));
            Assert.False(string.IsNullOrEmpty(Message(r)));
        }
        Assert.Equal(400, Status(c.SetPorts(null)));
        Assert.Equal(text, File.ReadAllText(SettingsPath));

        // The edges are fine.
        Assert.Equal(200, Status(c.SetPorts(new AdminPorts { Ports = Enumerable.Range(60000, 16).ToList() })));
        Assert.Equal(200, Status(c.SetPorts(new AdminPorts { Ports = new List<int> { 1024, 65535 } })));
    }

    [Fact]
    public void Ports_are_added_when_the_settings_have_none()
    {
        File.WriteAllText(SettingsPath, "{\n  \"Urls\": \"http://0.0.0.0:5000\"\n}\n");
        var rig = Make();
        Assert.Equal(new[] { 7777, 7778, 7779 }, Body<AdminStatus>(rig.Controller.GetStatus()).GamePorts);

        Assert.Equal(200, Status(rig.Controller.SetPorts(new AdminPorts { Ports = new List<int> { 7777 } })));

        var node = JsonNode.Parse(File.ReadAllText(SettingsPath))!;
        Assert.Equal("http://0.0.0.0:5000", (string)node["Urls"]!);
        Assert.Equal(new[] { 7777 }, node["GamePorts"]!.AsArray().Select(p => (int)p!));
    }

    [Fact]
    public void A_damaged_settings_file_is_left_alone()
    {
        File.WriteAllText(SettingsPath, "{ \"GamePorts\": [ 7777, ");
        var rig = Make();
        var r = rig.Controller.SetPorts(new AdminPorts { Ports = new List<int> { 7777 } });
        Assert.Equal(409, Status(r));
        Assert.Equal(SettingsWriter.Damaged, Message(r));
        Assert.Equal("{ \"GamePorts\": [ 7777, ", File.ReadAllText(SettingsPath));
    }

    static JsonNode WithoutPorts(string path)
    {
        var node = JsonNode.Parse(File.ReadAllText(path), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip })!.AsObject();
        node.Remove("GamePorts");
        return node;
    }
}
