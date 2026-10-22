using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace gamecontroller.Services
{
	public record UserEntry(string Name, string Key, bool CanCreateGames, bool ConfirmName, DateTime Added);

	// Who has an account on this server, and who may start games: reborn-users.json in the game's
	// Win64 folder. The game server reads the same file to check each joining player; this service
	// only needs the key and the "can create games" setting. Writers (tools/vm.py) replace the file
	// whole, so the list is simply re-read when the file's last-write time changes: adding or
	// removing a friend takes effect without a restart, and without ending running games.
	// Keys are never logged; names may be.
	public class UserList
	{
		public const string FileName = "reborn-users.json";

		// One complete read of the file; replaced as a whole so readers never see half a list.
		private sealed record Snapshot(bool Exists, bool RequireKeys, Dictionary<string, UserEntry> UsersByKey);

		private static readonly Snapshot NoFile = new(false, true, new Dictionary<string, UserEntry>(StringComparer.Ordinal));

		private readonly string _path;
		private readonly ILogger<UserList>? _logger;
		private readonly object _lock = new();
		private readonly Stopwatch _clock = Stopwatch.StartNew();
		private volatile Snapshot _current = NoFile;
		// Last-write time of the file as last applied; MinValue stands for "no file".
		private DateTime _stamp = DateTime.MinValue;
		private bool _failing;   // the file is there but unreadable right now
		private bool _warned;    // the "unreadable" warning has been logged for this bad spell
		private TimeSpan _lastCheck;

		// How often the file is looked at (tests set this to zero).
		public TimeSpan RecheckEvery { get; set; } = TimeSpan.FromSeconds(1);

		public UserList(IConfiguration config, ILogger<UserList>? logger = null)
		{
			_path = PathFor(config);
			_logger = logger;
			lock (_lock) Reload();
			_lastCheck = _clock.Elapsed;
		}

		// appsettings.json "UsersFile" if set, else reborn-users.json next to the game.
		public static string PathFor(IConfiguration config)
		{
			string? configured = config["UsersFile"];
			return !string.IsNullOrEmpty(configured) ? configured : Path.Combine(GameFolder.Resolve(config), FileName);
		}

		// The longest a player name may be (the same rule as tools/vm.py valid_name).
		public const int MaxNameLength = 24;

		// A name with everything but A-Z, a-z, 0-9, _ and - dropped, cut to MaxNameLength.
		// Used on names that arrive from elsewhere (a game server's poll) before they are kept or logged.
		public static string CleanName(string? name)
		{
			if (string.IsNullOrEmpty(name)) return "";
			var kept = new System.Text.StringBuilder(MaxNameLength);
			foreach (char c in name)
			{
				if (kept.Length == MaxNameLength) break;
				if (c is (>= 'A' and <= 'Z') or (>= 'a' and <= 'z') or (>= '0' and <= '9') or '_' or '-') kept.Append(c);
			}
			return kept.ToString();
		}

		public bool RequireKeys => Current.RequireKeys;
		public int Count => Current.UsersByKey.Count;
		public bool Exists => Current.Exists;

		public UserEntry? ByKey(string? key)
		{
			if (string.IsNullOrEmpty(key)) return null;
			return Current.UsersByKey.TryGetValue(key, out UserEntry? user) ? user : null;
		}

		private Snapshot Current
		{
			get
			{
				lock (_lock)
				{
					if (_clock.Elapsed - _lastCheck >= RecheckEvery)
					{
						_lastCheck = _clock.Elapsed;
						Reload();
					}
					return _current;
				}
			}
		}

		// Looks at the file and, if it changed (or could not be read last time), reads it again.
		private void Reload()
		{
			DateTime stamp;
			try { stamp = File.Exists(_path) ? File.GetLastWriteTimeUtc(_path) : DateTime.MinValue; }
			catch (IOException) { return; }
			if (stamp == _stamp && !_failing) return;

			if (stamp == DateTime.MinValue)
			{
				bool hadFile = _current.Exists;
				_current = NoFile;
				_stamp = stamp;
				_failing = _warned = false;
				if (hadFile) _logger?.LogWarning("The user list file is gone: nobody can start games");
				return;
			}

			Snapshot? fresh = TryRead(_path);
			if (fresh == null)
			{
				_failing = true;
				if (!_warned)
				{
					_warned = true;
					// The file is not echoed: a damaged copy may still contain keys.
					if (_current.Exists) _logger?.LogWarning("User list unreadable; keeping the previous list");
					else
					{
						_current = new Snapshot(true, true, new Dictionary<string, UserEntry>(StringComparer.Ordinal));
						_logger?.LogWarning("User list unreadable; nobody can start games until it is fixed");
					}
				}
				return;
			}

			bool first = !_current.Exists;
			_current = fresh;
			_stamp = stamp;
			_failing = _warned = false;
			// Names only: the keys themselves never go into the log.
			_logger?.LogInformation("{Verb} the user list: {Count} users ({Names}), keys {Keys}", first ? "Loaded" : "Reloaded",
				fresh.UsersByKey.Count, string.Join(", ", fresh.UsersByKey.Values.Select(u => u.Name).OrderBy(n => n, StringComparer.OrdinalIgnoreCase)),
				fresh.RequireKeys ? "required" : "not required");
		}

		// Null when the file cannot be read or is not the expected shape.
		private static Snapshot? TryRead(string path)
		{
			try
			{
				using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
				using JsonDocument doc = JsonDocument.Parse(stream);
				JsonElement root = doc.RootElement;
				if (root.ValueKind != JsonValueKind.Object) return null;

				// The same strict shape as the game server (reborn/AccessRules.hpp) and tools/vm.py:
				// Name and Key non-empty strings; RequireKeys, CanCreateGames and ConfirmName optional
				// but true/false when present. Anything else makes the whole file unreadable.
				if (!OptionalBool(root, "RequireKeys", true, out bool requireKeys)) return null;   // missing means required

				if (!root.TryGetProperty("Users", out JsonElement list) || list.ValueKind != JsonValueKind.Array) return null;
				var byKey = new Dictionary<string, UserEntry>(StringComparer.Ordinal);
				foreach (JsonElement item in list.EnumerateArray())
				{
					if (item.ValueKind != JsonValueKind.Object) return null;
					string? name = StringOf(item, "Name"), key = StringOf(item, "Key");
					if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(key)) return null;
					if (!OptionalBool(item, "CanCreateGames", false, out bool canCreate)) return null;
					if (!OptionalBool(item, "ConfirmName", false, out bool confirmName)) return null;
					byKey.TryAdd(key, new UserEntry(name, key, canCreate, confirmName, DateOf(item, "Added")));
				}
				return new Snapshot(true, requireKeys, byKey);
			}
			catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException)
			{
				return null;
			}
		}

		private static string? StringOf(JsonElement obj, string name) =>
			obj.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

		// False when the property is there but is not a JSON true/false; a missing one gives `fallback`.
		private static bool OptionalBool(JsonElement obj, string name, bool fallback, out bool value)
		{
			value = fallback;
			if (!obj.TryGetProperty(name, out JsonElement v)) return true;
			if (v.ValueKind != JsonValueKind.True && v.ValueKind != JsonValueKind.False) return false;
			value = v.GetBoolean();
			return true;
		}

		private static DateTime DateOf(JsonElement obj, string name) =>
			DateTime.TryParse(StringOf(obj, name), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out DateTime d)
				? d : DateTime.MinValue;

		// Run once at startup. The first time the service sees StartKeys but no user list, the keys
		// become users (who must keep their old start keys working: ConfirmName, so the label need not
		// be their player name). An existing file is never touched. Returns true if it wrote the file.
		public static bool Migrate(IConfiguration config, string path, ILogger? logger)
		{
			if (File.Exists(path)) return false;

			var users = new List<(string Label, string Key)>();
			foreach (var child in config.GetSection("StartKeys").GetChildren())
				if (!string.IsNullOrEmpty(child.Value) && users.All(u => u.Key != child.Value)) users.Add((child.Key, child.Value));

			if (users.Count == 0)
			{
				logger?.LogInformation("No user list: anyone can join, nobody can start games");
				return false;
			}

			string tmp = path + ".tmp";
			try
			{
				string added = DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
				string json = JsonSerializer.Serialize(new
				{
					RequireKeys = true,
					Users = users.Select(u => new { Name = u.Label, Key = u.Key, CanCreateGames = true, ConfirmName = true, Added = added }),
				}, new JsonSerializerOptions { WriteIndented = true });
				string? dir = Path.GetDirectoryName(path);
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				File.WriteAllText(tmp, json);
				File.Move(tmp, path, overwrite: false);   // throws if the file appeared meanwhile: never overwrite
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				try { File.Delete(tmp); } catch { }
				logger?.LogWarning("Could not create the user list: {Error}", e.GetType().Name);
				return false;
			}
			// Labels only: the keys themselves never go into the log.
			logger?.LogInformation("Created the user list from {Count} start keys ({Labels})", users.Count, string.Join(", ", users.Select(u => u.Label)));
			// While StartKeys stays, a missing user list is rebuilt from it, bringing removed people back.
			logger?.LogWarning("Now delete the StartKeys block from appsettings.json: the user list replaces it");
			return true;
		}
	}
}
