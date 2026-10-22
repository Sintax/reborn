using System.Net;
using System.Net.Http.Json;
using System.Text.Json;

namespace ControlPanel.Core
{
	// What one call to matchmaking came to. Ok, or a plain Message for the owner: the API's own words,
	// or "Matchmaking is not running." when it could not be reached (NotRunning).
	public class ApiResult
	{
		public bool Ok { get; }
		public string Message { get; }
		public bool NotRunning { get; }

		public ApiResult(bool ok, string message, bool notRunning = false)
		{
			Ok = ok;
			Message = message;
			NotRunning = notRunning;
		}
	}

	public sealed class ApiResult<T> : ApiResult
	{
		public T? Value { get; }

		public ApiResult(T value) : base(true, "") => Value = value;
		public ApiResult(ApiResult failed) : base(false, failed.Message, failed.NotRunning) { }
	}

	// The shapes gamecontroller/Controllers/Admin.cs sends (camelCase JSON).
	public sealed record ServerStatus(string Version, string? PublicIp, IReadOnlyList<int> GamePorts, IReadOnlyList<int> GamePortsInUse,
		int GamesRunning, bool RequireKeys, DateTime MatchmakingStartedUtc);

	public sealed record PlayerInfo(string Name, string Hero, int Team);

	public sealed record GameInfo(string PublicId, string Name, string MapMode, string DraftStyle, bool RandomHero, int Port,
		DateTime StartedUtc, bool MatchStarted, bool IsRoster, string? StartedBy, IReadOnlyList<PlayerInfo>? Players);

	public sealed record UserInfo(string Name, bool CanCreateGames, bool ConfirmName, DateTime? Added, DateTime? LastSeen);

	public sealed record UserListInfo(bool RequireKeys, IReadOnlyList<UserInfo> Users);

	// The local admin API of matchmaking (http://127.0.0.1:<CoordinatorPort>/api/admin/). Every call
	// sends the token from admin-token.txt and returns a result; nothing here throws to the UI.
	public sealed class AdminClient
	{
		public const string NotRunningMessage = "Matchmaking is not running.";
		public const string NotAnsweringMessage = "Matchmaking is not answering. It may be starting or stuck; wait a moment.";
		public const string SignInMessage = "The Control Panel could not sign in to matchmaking. Restart matchmaking.";
		public const string UnreadableMessage = "Matchmaking sent an answer the Control Panel could not read. Is the Control Panel the same version as matchmaking?";

		private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web)
		{
			DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,   // a field left out = "no change"
		};

		private readonly HttpClient _http;
		private readonly Func<string?> _token;
		private readonly Uri _base;

		// baseUri: the admin API's address (PanelPaths.AdminBaseUri); defaults to http.BaseAddress,
		// then http://127.0.0.1:5000/api/admin/.
		public AdminClient(HttpClient http, Func<string?> token, Uri? baseUri = null)
		{
			_http = http;
			_token = token;
			_base = baseUri ?? http.BaseAddress ?? new Uri("http://127.0.0.1:5000/api/admin/");
		}

		// The panel's own client: short timeout so a stuck matchmaking never freezes the window.
		public static AdminClient For(PanelPaths paths) =>
			new(new HttpClient { Timeout = TimeSpan.FromSeconds(4) }, paths.ReadToken, paths.AdminBaseUri);

		private static string Name(string name) => Uri.EscapeDataString(name);

		public Task<ApiResult<ServerStatus>> GetStatusAsync() => Send<ServerStatus>(HttpMethod.Get, "status", check: Checked);

		public Task<ApiResult<IReadOnlyList<GameInfo>>> GetGamesAsync() => Send<IReadOnlyList<GameInfo>>(HttpMethod.Get, "games", check: Checked);

		public Task<ApiResult<UserListInfo>> GetUsersAsync() => Send<UserListInfo>(HttpMethod.Get, "users", check: Checked);

		// Only for making a friend's zip. The key is never logged or shown.
		public async Task<ApiResult<string>> GetKeyAsync(string name)
		{
			var r = await Send<KeyReply>(HttpMethod.Get, $"users/{Name(name)}/key", check: k => k.Key == null ? null : k);
			return r.Ok ? new ApiResult<string>(r.Value!.Key) : new ApiResult<string>(r);
		}

		public Task<ApiResult<UserInfo>> AddUserAsync(string name, string key, bool canCreateGames) =>
			Send<UserInfo>(HttpMethod.Post, "users", new { name, key, canCreateGames }, Checked);

		public Task<ApiResult<UserInfo>> UpdateUserAsync(string name, string? newName = null, bool? canCreateGames = null) =>
			Send<UserInfo>(HttpMethod.Put, $"users/{Name(name)}", new { newName, canCreateGames }, Checked);

		public Task<ApiResult> ReplaceKeyAsync(string name, string key) => SendNoReply(HttpMethod.Put, $"users/{Name(name)}/key", new { key });

		public Task<ApiResult> RemoveUserAsync(string name) => SendNoReply(HttpMethod.Delete, $"users/{Name(name)}");

		public async Task<ApiResult<bool>> SetRequireKeysAsync(bool on)
		{
			var r = await Send<RequireKeysReply>(HttpMethod.Put, "require-keys", new { on });
			return r.Ok ? new ApiResult<bool>(r.Value!.RequireKeys) : new ApiResult<bool>(r);
		}

		// Saved for matchmaking's next start; nothing running is stopped.
		public Task<ApiResult> SetPortsAsync(IEnumerable<int> ports) => SendNoReply(HttpMethod.Put, "ports", new { ports = ports.ToList() });

		// --- answers from another matchmaking version ---
		// JSON may leave out fields this panel expects (an older or newer matchmaking). A missing list
		// the panel can't do without makes the answer unreadable (null); anything else gets a safe
		// default, so nothing further on meets a null.

		private static ServerStatus? Checked(ServerStatus s) => s.GamePorts == null ? null : s with
		{
			Version = s.Version ?? "",
			GamePortsInUse = s.GamePortsInUse ?? s.GamePorts,   // before gamePortsInUse existed: no restart pending
		};

		private static IReadOnlyList<GameInfo>? Checked(IReadOnlyList<GameInfo> games) => games
			.Where(g => g != null && g.PublicId != null)
			.Select(g => g with
			{
				Name = g.Name ?? "",
				MapMode = g.MapMode ?? "",
				DraftStyle = g.DraftStyle ?? "",
				Players = (g.Players ?? Array.Empty<PlayerInfo>())
					.Where(p => p != null)
					.Select(p => p with { Name = p.Name ?? "", Hero = p.Hero ?? "" })
					.ToList(),
			})
			.ToList();

		private static UserListInfo? Checked(UserListInfo list) => list.Users == null ? null : list with
		{
			Users = list.Users.Where(u => u != null && u.Name != null).ToList(),
		};

		private static UserInfo? Checked(UserInfo u) => u.Name == null ? null : u;

		private sealed record KeyReply(string Key);
		private sealed record RequireKeysReply(bool RequireKeys);

		private async Task<ApiResult> SendNoReply(HttpMethod method, string route, object? body = null)
		{
			var (response, failed) = await Exchange(method, route, body);
			using (response) return failed ?? new ApiResult(true, "");
		}

		private async Task<ApiResult<T>> Send<T>(HttpMethod method, string route, object? body = null, Func<T, T?>? check = null)
		{
			var (response, failed) = await Exchange(method, route, body);
			using (response)
			{
				if (failed != null) return new ApiResult<T>(failed);
				try
				{
					T? value = await response!.Content.ReadFromJsonAsync<T>(Json);
					if (value != null && check != null) value = check(value);
					return value == null ? new ApiResult<T>(new ApiResult(false, UnreadableMessage)) : new ApiResult<T>(value);
				}
				catch (Exception e) when (e is JsonException or NotSupportedException or HttpRequestException or IOException
					or OperationCanceledException or InvalidOperationException)
				{
					return new ApiResult<T>(new ApiResult(false, UnreadableMessage));
				}
			}
		}

		// The response when it was a success, else why not (and no response).
		private async Task<(HttpResponseMessage? response, ApiResult? failed)> Exchange(HttpMethod method, string route, object? body)
		{
			HttpResponseMessage response;
			try
			{
				using var request = new HttpRequestMessage(method, new Uri(_base, route));
				string? token = _token();
				if (!string.IsNullOrEmpty(token)) request.Headers.TryAddWithoutValidation("X-Admin-Token", token);
				if (body != null) request.Content = JsonContent.Create(body, options: Json);
				response = await _http.SendAsync(request);
			}
			catch (HttpRequestException)
			{
				return (null, new ApiResult(false, NotRunningMessage, notRunning: true));
			}
			catch (OperationCanceledException)   // the timeout
			{
				return (null, new ApiResult(false, NotAnsweringMessage, notRunning: true));
			}
			catch (Exception e) when (e is InvalidOperationException or IOException or UriFormatException)
			{
				return (null, new ApiResult(false, NotRunningMessage, notRunning: true));
			}

			if (response.IsSuccessStatusCode) return (response, null);
			using (response)
			{
				if (response.StatusCode == HttpStatusCode.Forbidden) return (null, new ApiResult(false, SignInMessage));
				string? message = null;
				try
				{
					using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
					if (doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("message", out JsonElement m)
						&& m.ValueKind == JsonValueKind.String)
						message = m.GetString();
				}
				catch (Exception e) when (e is JsonException or HttpRequestException or IOException or OperationCanceledException) { }
				return (null, new ApiResult(false, string.IsNullOrWhiteSpace(message)
					? $"Matchmaking could not do that (error {(int)response.StatusCode}). Try again."
					: message!));
			}
		}
	}
}
