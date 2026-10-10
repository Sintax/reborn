using Microsoft.AspNetCore.Mvc;
using gamecontroller.Middleware;
using System.Diagnostics;
using gamecontroller.Singletons;
using gamecontroller.Models;

namespace gamecontroller.Controllers
{
    // The game's server browser reads the list here; game servers report in with their X-Server-Token.
    // Servers come from the roster (ServerRoster.cs) or, for the lobby flow, from a lobby's GameInstance.

    [ApiController]
    [Route("api/games")]
    public class Game : ControllerBase
    {
        private readonly ILogger<Game> _logger;

        private readonly LobbySingleton _lobbySingleton;

        private readonly ServerRoster _roster;

        public Game(ILogger<Game> logger, LobbySingleton lobbySingleton, ServerRoster roster)
        {
            _logger = logger;
            _lobbySingleton = lobbySingleton;
            _roster = roster;
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

        private Lobby? LobbyForToken(string token)
        {
            return _lobbySingleton.Lobbies.FirstOrDefault(l => l.GameInstance != null && l.GameInstance.MyGuid.Equals(token));
        }

        [HttpGet("server-config")]
        public GameCreationConfig? GetServerConfig()
        {
            string? token = ServerToken();
            if (token != null)
            {
                GameCreationConfig? config = _roster.ConfigFor(token) ?? LobbyForToken(token)?.GameInstance?.Config;
                if (config != null)
                {
                    config.Port = _roster.Instances.FirstOrDefault(i => i.MyGuid == token)?.Port
                        ?? LobbyForToken(token)?.GameInstance?.Port ?? 7777;
                    return config;
                }
            }

            HttpContext.Response.StatusCode = 401;
            return null;
        }

        [HttpPost("server-allow-player-join")]
        public void PostServerPoll([FromBody] AllowPlayerJoin playerJoin)
        {
            string? token = ServerToken();
            Lobby? lobby = token != null ? LobbyForToken(token) : null;
            if (lobby != null)
            {
                lobby.PlayerIndexToAllowJoin = playerJoin.PlayerIndex;
                lobby.AllowJoin = true;

                HttpContext.Response.StatusCode = 200;
                return;
            }

            HttpContext.Response.StatusCode = 401;
            return;
        }

        [HttpPost("server-match-natural-shutdown")]
        public void ServerMatchNaturalShutdown()
        {
            string? token = ServerToken();
            if (token != null)
            {
                if (_roster.MarkFinished(token))
                {
                    HttpContext.Response.StatusCode = 200;
                    return;
                }

                Lobby? lobby = LobbyForToken(token);
                if (lobby != null)
                {
                    lobby.MatchShutdown();

                    HttpContext.Response.StatusCode = 200;
                    return;
                }
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
                if (_roster.RecordPoll(token, body.ConnectedPlayers, body.HumansHaveStarted, DateTime.UtcNow)) return;

                Lobby? lobby = LobbyForToken(token);
                if (lobby != null && lobby.GameInstance != null)
                {
                    lobby.GameInstance.LastServerCheckIn = DateTime.UtcNow;
                }
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
