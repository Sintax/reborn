using Microsoft.AspNetCore.Mvc;
using gamecontroller.Singletons;
using gamecontroller.Models;
using gamecontroller.Services;

namespace gamecontroller.Controllers
{
    // The game's server browser reads the list here; game servers report in with their X-Server-Token.
    // Every server comes from the roster (ServerRoster.cs): the standing ones and the ones players start.

    [ApiController]
    [Route("api/games")]
    public class Game : ControllerBase
    {
        private readonly ILogger<Game> _logger;

        private readonly ServerRoster _roster;

        private readonly UserList _users;

        private readonly StinkyWordService _words;

        public Game(ILogger<Game> logger, ServerRoster roster, UserList users, StinkyWordService words)
        {
            _logger = logger;
            _roster = roster;
            _users = users;
            _words = words;
        }

        // The user whose key came with the request, or the refusal to send back: 401 when the key is
        // missing or unknown, 403 when the user may not start games.
        private (UserEntry? User, IActionResult? Refusal) StartingUser()
        {
            UserEntry? user = _users.ByKey(HttpContext.Request.Headers.TryGetValue("X-Start-Key", out var k) ? k.ToString() : null);
            if (user == null) return (null, Unauthorized(new { message = "You need a server key from the host." }));
            if (!user.CanCreateGames) return (null, StatusCode(403, new { message = "Your account can't start games on this server." }));
            return (user, null);
        }

        // A player with a key file starts a game from the server browser (GameCoordinator.cpp StartGame).
        [HttpPost("start")]
        public IActionResult Start([FromBody] GameRequest req)
        {
            var (user, refusal) = StartingUser();
            if (user == null) return refusal!;

            string name = (req.Name ?? "").Trim();
            if (name.Length < 1 || name.Length > 40) return BadRequest(new { message = "Pick a game name of 1 to 40 characters." });
            if (_words.StringContainsNoNoWords(name)) return BadRequest(new { message = "That game name is not allowed." });
            req.Name = name;

            StartResult result = _roster.StartRequested(req, user.Name, DateTime.UtcNow);
            if (!result.Ok)
            {
                int status = result.Error.StartsWith("Server is full") ? 409 : 400;
                return StatusCode(status, new { message = result.Error });
            }
            var instance = _roster.Instances.First(i => i.PublicId == result.Guid);
            _logger.LogInformation("{Label} ({Player}) started \"{Name}\" ({MapMode}) on port {Port}: {Command}",
                user.Name, req.PlayerName, name, req.MapMode, instance.Port, instance.Config.ServerStartupCommand);
            return Ok(new { guid = result.Guid, connectString = instance.ConnectionString });
        }

        [HttpDelete("{guid}")]
        public IActionResult Cancel(string guid)
        {
            var (user, refusal) = StartingUser();
            if (user == null) return refusal!;
            if (!_roster.Instances.Any(i => i.PublicId == guid && i.Slot == null && i.KeyLabel == user.Name)) return NotFound(new { message = "No such game." });
            _roster.Cancel(guid);
            _logger.LogInformation("{Label} cancelled game {Guid}", user.Name, guid);
            return Ok();
        }

        [HttpGet]
        public List<ServerBrowserEntry> GetGames()
        {
            return _roster.BrowserEntries();
        }

        private string? ServerToken()
        {
            return HttpContext.Request.Headers.TryGetValue("X-Server-Token", out var authHeader) ? authHeader.ToString() : null;
        }

        [HttpGet("server-config")]
        public GameCreationConfig? GetServerConfig()
        {
            string? token = ServerToken();
            if (token != null)
            {
                GameCreationConfig? config = _roster.ConfigFor(token);
                if (config != null)
                {
                    config.Port = _roster.Instances.FirstOrDefault(i => i.MyGuid == token)?.Port ?? 7777;
                    return config;
                }
            }

            HttpContext.Response.StatusCode = 401;
            return null;
        }

        [HttpPost("server-match-natural-shutdown")]
        public void ServerMatchNaturalShutdown()
        {
            string? token = ServerToken();
            if (token != null && _roster.MarkFinished(token))
            {
                HttpContext.Response.StatusCode = 200;
                return;
            }

            HttpContext.Response.StatusCode = 401;
            return;
        }

        [HttpPost("server-poll")]
        public void ServerPoll([FromBody] ServerPollBody body)
        {
            string? token = ServerToken();
            if (token != null)
            {
                _roster.RecordPoll(token, body.ConnectedPlayers, body.HumansHaveStarted, DateTime.UtcNow);
            }
        }
    }

    // What the game server sends every five seconds (Networking.cpp GameControllerPoll).
    public class ServerPollBody
    {
        public int ConnectedPlayers { get; set; }
        public bool HumansHaveStarted { get; set; }
    }
}
