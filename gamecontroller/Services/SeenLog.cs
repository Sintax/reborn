using System.Globalization;
using System.Text.Json;

namespace gamecontroller.Services
{
	// When each player name was last seen in a running game: reborn-users-seen.json next to the user
	// list, {"Seen":{"Sintax":"2026-10-22T18:00:00Z"}}. Names match case-insensitively. The file is
	// written at most once a minute and once more at shutdown, always whole (.tmp then rename), so a
	// crash loses at most a minute and never leaves a half-written file. A missing or unreadable file
	// simply starts an empty list.
	public sealed class SeenLog : IDisposable
	{
		public const string FileName = "reborn-users-seen.json";
		public static readonly TimeSpan SaveEvery = TimeSpan.FromMinutes(1);

		private readonly string _path;
		private readonly Func<DateTime> _now;
		private readonly ILogger? _logger;
		private readonly object _lock = new();
		private readonly Dictionary<string, DateTime> _seen = new(StringComparer.OrdinalIgnoreCase);
		private DateTime? _lastSave;
		private bool _dirty;
		private bool _saveWarned;

		public SeenLog(string path, Func<DateTime> now, ILogger<SeenLog>? logger = null)
		{
			_path = path;
			_now = now;
			_logger = logger;
			Load();
		}

		// Next to the user list: the game's Win64 folder unless appsettings.json "UsersFile" says otherwise.
		public static string PathFor(IConfiguration config) =>
			Path.Combine(Path.GetDirectoryName(Path.GetFullPath(UserList.PathFor(config))) ?? "", FileName);

		public void Saw(IEnumerable<string> names)
		{
			lock (_lock)
			{
				DateTime now = _now();
				foreach (string name in names)
				{
					if (string.IsNullOrEmpty(name)) continue;
					_seen.Remove(name);   // so the file keeps the name as it was last written
					_seen[name] = now;
					_dirty = true;
				}
				if (_dirty && (_lastSave == null || now - _lastSave.Value >= SaveEvery)) Save(now);
			}
		}

		// A user was renamed: their last-seen time moves to the new name (the later one wins if the new
		// name was seen too). Saved on the same once-a-minute rule as Saw, and at shutdown.
		public void Rename(string oldName, string newName)
		{
			if (string.IsNullOrEmpty(oldName) || string.IsNullOrEmpty(newName)) return;
			lock (_lock)
			{
				if (!_seen.TryGetValue(oldName, out DateTime when)) return;
				_seen.Remove(oldName);
				if (_seen.TryGetValue(newName, out DateTime other) && other > when) when = other;
				_seen.Remove(newName);   // so the file keeps the new spelling
				_seen[newName] = when;
				_dirty = true;
				DateTime now = _now();
				if (_lastSave == null || now - _lastSave.Value >= SaveEvery) Save(now);
			}
		}

		public DateTime? LastSeen(string name)
		{
			lock (_lock) return _seen.TryGetValue(name, out DateTime when) ? when : null;
		}

		// Every name seen, with when, as a copy (names only: this log never holds keys).
		public IReadOnlyList<(string Name, DateTime When)> All()
		{
			lock (_lock) return _seen.Select(kv => (kv.Key, kv.Value)).ToList();
		}

		// Shutdown: whatever is not yet in the file goes there now.
		public void Dispose()
		{
			lock (_lock) if (_dirty) Save(_now());
		}

		private void Load()
		{
			if (!File.Exists(_path))
			{
				_logger?.LogInformation("No list of when players were last seen yet; starting one");
				return;
			}
			try
			{
				using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(_path));
				if (doc.RootElement.ValueKind != JsonValueKind.Object
					|| !doc.RootElement.TryGetProperty("Seen", out JsonElement seen) || seen.ValueKind != JsonValueKind.Object)
					throw new JsonException("not the expected shape");
				foreach (JsonProperty p in seen.EnumerateObject())
				{
					string name = UserList.CleanName(p.Name);
					if (name == "" || p.Value.ValueKind != JsonValueKind.String) continue;
					if (DateTime.TryParse(p.Value.GetString(), CultureInfo.InvariantCulture,
						DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out DateTime when))
						_seen[name] = when;
				}
				_logger?.LogInformation("Loaded when {Count} players were last seen", _seen.Count);
			}
			catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
			{
				_seen.Clear();
				_logger?.LogWarning("The list of when players were last seen could not be read ({Error}); starting a new one", e.GetType().Name);
			}
		}

		private void Save(DateTime now)
		{
			_lastSave = now;
			string tmp = _path + ".tmp";
			try
			{
				string json = JsonSerializer.Serialize(new
				{
					Seen = _seen.ToDictionary(kv => kv.Key, kv => kv.Value.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture)),
				}, new JsonSerializerOptions { WriteIndented = true });
				string? dir = Path.GetDirectoryName(_path);
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				File.WriteAllText(tmp, json);
				File.Move(tmp, _path, overwrite: true);
				_dirty = false;
				_saveWarned = false;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				try { File.Delete(tmp); } catch { }
				if (!_saveWarned) _logger?.LogWarning("Could not save when players were last seen: {Error}", e.GetType().Name);
				_saveWarned = true;
			}
		}
	}
}
