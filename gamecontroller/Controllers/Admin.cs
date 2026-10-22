using System.Net;
using gamecontroller.Services;
using gamecontroller.Singletons;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.Filters;

namespace gamecontroller.Controllers
{
	// The local admin API the Control Panel on the server machine uses: status, games, users and
	// ports. Only the same machine may call it, and only with the token from admin-token.txt
	// (AdminGuard below); everyone else gets a bare 403. Keys are never logged and never listed: only
	// GET users/{name}/key hands one out (the panel needs it to make a friend's zip).
	[ApiController]
	[Route("api/admin")]
	[ServiceFilter(typeof(AdminGuard))]
	public class Admin : ControllerBase
	{
		private readonly ServerRoster _roster;
		private readonly UserList _users;
		private readonly UserListWriter _writer;
		private readonly SettingsWriter _settings;
		private readonly SeenLog _seen;
		private readonly IConfiguration _config;
		private readonly ServiceStarted _started;
		private readonly ILogger<Admin> _logger;

		public Admin(ServerRoster roster, UserList users, UserListWriter writer, SettingsWriter settings, SeenLog seen,
			IConfiguration config, ServiceStarted started, ILogger<Admin> logger)
		{
			_roster = roster;
			_users = users;
			_writer = writer;
			_settings = settings;
			_seen = seen;
			_config = config;
			_started = started;
			_logger = logger;
		}

		private ObjectResult Refused(AdminRefusal refusal) => StatusCode(refusal.Status, new { message = refusal.Message });

		private static IActionResult? Missing(object? body) => body == null ? new BadRequestObjectResult(new { message = "The request was empty." }) : null;

		private AdminUser Shown(StoredUser u) => new(u.Name, u.CanCreateGames, u.ConfirmName, u.Added, _seen.LastSeen(u.Name));

		[HttpGet("status")]
		public IActionResult GetStatus()
		{
			string version = "";
			try
			{
				string file = Path.Combine(GameFolder.Resolve(_config), "reborn-deployed.txt");
				if (System.IO.File.Exists(file)) version = System.IO.File.ReadAllText(file).Trim();
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }

			bool requireKeys = _writer.Read(out UserFileView? view) == null ? view!.RequireKeys : _users.RequireKeys;
			IReadOnlyList<int> ports = _settings.ReadPorts()
				?? _config.GetSection("GamePorts").Get<List<int>>() ?? SettingsWriter.DefaultPorts;
			return Ok(new AdminStatus(version, _roster.PublicIp, ports, _roster.Ports.ToList(), _roster.Instances.Count(i => !i.Finished),
				requireKeys, _started.Utc));
		}

		[HttpGet("games")]
		public IActionResult GetGames() => Ok(_roster.Instances.Where(i => !i.Finished).Select(i => new AdminGame(
			i.PublicId, i.Config.InstanceName, i.Config.HumanReadableInstanceMapMode, i.Config.DraftStyle, i.Config.RandomHero, i.Port,
			i.CreationTime, i.MatchStarted, i.Slot != null, i.KeyLabel,
			i.Players.Select(p => new AdminPlayer(p.Name, p.Hero, p.Team)).ToList())).ToList());

		[HttpGet("users")]
		public IActionResult GetUsers()
		{
			AdminRefusal? refusal = _writer.Read(out UserFileView? view);
			if (refusal != null) return Refused(refusal);
			return Ok(new AdminUsers(view!.RequireKeys, view.Users.Select(Shown).ToList()));
		}

		[HttpGet("users/{name}/key")]
		public IActionResult GetKey(string name)
		{
			AdminRefusal? refusal = _writer.Read(out UserFileView? view);
			if (refusal != null) return Refused(refusal);
			StoredUser? user = UserListWriter.FindIn(view!, name);
			if (user == null) return NotFound(new { message = UserListWriter.NoSuchUser });
			_logger.LogInformation("Admin: the key of {Name} was read", user.Name);
			return Ok(new AdminKeyReply(user.Key));
		}

		[HttpPost("users")]
		public IActionResult AddUser([FromBody] AdminNewUser? body)
		{
			if (Missing(body) is IActionResult bad) return bad;
			AdminRefusal? refusal = _writer.Add(body!.Name, body.Key, body.CanCreateGames ?? false, DateTime.UtcNow, out StoredUser? added);
			if (refusal != null) return Refused(refusal);
			_logger.LogInformation("Admin: added user {Name} (can start games: {Create})", added!.Name, added.CanCreateGames ? "yes" : "no");
			return StatusCode(201, Shown(added));
		}

		[HttpPut("users/{name}")]
		public IActionResult UpdateUser(string name, [FromBody] AdminUserChange? body)
		{
			if (Missing(body) is IActionResult bad) return bad;
			AdminRefusal? refusal = _writer.Update(name, body!.NewName, body.CanCreateGames, out StoredUser? updated, out string? oldName);
			if (refusal != null) return Refused(refusal);
			if (body.NewName != null)
			{
				_seen.Rename(oldName!, updated!.Name);   // "last seen" follows the user to the new name
				_logger.LogInformation("Admin: renamed user {Old} to {New}", oldName, updated.Name);
			}
			if (body.CanCreateGames != null)
				_logger.LogInformation("Admin: {Name} can start games: {Create}", updated!.Name, updated.CanCreateGames ? "yes" : "no");
			return Ok(Shown(updated!));
		}

		[HttpPut("users/{name}/key")]
		public IActionResult ReplaceKey(string name, [FromBody] AdminKey? body)
		{
			if (Missing(body) is IActionResult bad) return bad;
			AdminRefusal? refusal = _writer.ReplaceKey(name, body!.Key, out string? userName);
			if (refusal != null) return Refused(refusal);
			_logger.LogInformation("Admin: gave {Name} a new key", userName);
			return NoContent();
		}

		[HttpDelete("users/{name}")]
		public IActionResult RemoveUser(string name)
		{
			AdminRefusal? refusal = _writer.Remove(name, out string? removed);
			if (refusal != null) return Refused(refusal);
			_logger.LogInformation("Admin: removed user {Name}", removed);
			return NoContent();
		}

		[HttpPut("require-keys")]
		public IActionResult SetRequireKeys([FromBody] AdminRequireKeys? body)
		{
			if (Missing(body) is IActionResult bad) return bad;
			if (body!.On == null) return BadRequest(new { message = "Say whether keys are required: on or off." });
			AdminRefusal? refusal = _writer.SetRequireKeys(body.On.Value);
			if (refusal != null) return Refused(refusal);
			_logger.LogInformation("Admin: keys {State}", body.On.Value ? "required" : "not required");
			return Ok(new { requireKeys = body.On.Value });
		}

		// Saved for the next start only: the running service keeps its ports, and no game is stopped.
		[HttpPut("ports")]
		public IActionResult SetPorts([FromBody] AdminPorts? body)
		{
			if (Missing(body) is IActionResult bad) return bad;
			AdminRefusal? refusal = _settings.SavePorts(body!.Ports);
			if (refusal != null) return Refused(refusal);
			_logger.LogInformation("Admin: game ports saved for the next start: {Ports}", string.Join(", ", body.Ports!));
			return Ok(new { restartNeeded = true });
		}
	}

	// Lets a request into the admin API only when it comes from this machine and carries the token.
	// An authorization filter, so it runs before the request body is even read: a caller without
	// both gets 403 with nothing that says which part was wrong.
	public sealed class AdminGuard : IAuthorizationFilter
	{
		public const string Header = "X-Admin-Token";

		private readonly AdminToken _token;

		public AdminGuard(AdminToken token) => _token = token;

		public bool Allows(HttpContext http)
		{
			// Both ends of the connection must be this machine. The arriving-on address still holds if
			// forwarded headers were ever switched on (they would rewrite only the caller's address).
			if (!IsLoopback(http.Connection.RemoteIpAddress) || !IsLoopback(http.Connection.LocalIpAddress)) return false;
			// Anything that came through a proxy is refused, whatever address it claims.
			if (http.Request.Headers.ContainsKey("X-Forwarded-For") || http.Request.Headers.ContainsKey("Forwarded")) return false;
			if (!http.Request.Headers.TryGetValue(Header, out var values) || values.Count != 1) return false;
			return _token.Matches(values[0]);
		}

		private static bool IsLoopback(IPAddress? address)
		{
			if (address == null) return false;
			if (address.IsIPv4MappedToIPv6) address = address.MapToIPv4();
			return IPAddress.IsLoopback(address);
		}

		public void OnAuthorization(AuthorizationFilterContext context)
		{
			if (!Allows(context.HttpContext)) context.Result = new BareForbidden();
		}

		// 403 with an empty body. Not a StatusCodeResult: [ApiController] would dress that up as a
		// problem-details reply.
		private sealed class BareForbidden : IActionResult
		{
			public Task ExecuteResultAsync(ActionContext context)
			{
				context.HttpContext.Response.StatusCode = 403;
				return Task.CompletedTask;
			}
		}
	}

	// When this run of the service started (for the panel's status line).
	public sealed record ServiceStarted(DateTime Utc);

	// GamePorts: saved in appsettings.json (what the next start uses). GamePortsInUse: what this run
	// started with. They differ after a ports change until matchmaking restarts.
	public sealed record AdminStatus(string Version, string? PublicIp, IReadOnlyList<int> GamePorts, IReadOnlyList<int> GamePortsInUse,
		int GamesRunning, bool RequireKeys, DateTime MatchmakingStartedUtc);

	public sealed record AdminPlayer(string Name, string Hero, int Team);

	public sealed record AdminGame(string PublicId, string Name, string MapMode, string DraftStyle, bool RandomHero, int Port,
		DateTime StartedUtc, bool MatchStarted, bool IsRoster, string? StartedBy, IReadOnlyList<AdminPlayer> Players);

	public sealed record AdminUser(string Name, bool CanCreateGames, bool ConfirmName, DateTime? Added, DateTime? LastSeen);

	public sealed record AdminUsers(bool RequireKeys, IReadOnlyList<AdminUser> Users);

	public sealed record AdminKeyReply(string Key);

	// Request bodies. Every field may be missing so the API can answer with its own plain message.
	public sealed class AdminNewUser
	{
		public string? Name { get; set; }
		public string? Key { get; set; }
		public bool? CanCreateGames { get; set; }
	}

	public sealed class AdminUserChange
	{
		public string? NewName { get; set; }
		public bool? CanCreateGames { get; set; }
	}

	public sealed class AdminKey
	{
		public string? Key { get; set; }
	}

	public sealed class AdminRequireKeys
	{
		public bool? On { get; set; }
	}

	public sealed class AdminPorts
	{
		public List<int>? Ports { get; set; }
	}
}
