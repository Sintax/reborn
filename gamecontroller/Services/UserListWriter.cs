using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace gamecontroller.Services
{
	// Why a change was not made: the HTTP status and a plain sentence for the owner.
	public sealed record AdminRefusal(int Status, string Message);

	// One user as stored, key included: only the admin API's key call ever hands the key out.
	public sealed record StoredUser(string Name, string Key, bool CanCreateGames, bool ConfirmName, DateTime? Added);

	public sealed record UserFileView(bool RequireKeys, IReadOnlyList<StoredUser> Users);

	// Changes reborn-users.json for the admin API: read the file in the strict shape every reader accepts
	// (UserList.cs, reborn/AccessRules.hpp, tools/vm.py), change it, write it whole (.tmp then rename).
	// A file that shape check calls damaged is never written over, so nothing in it is lost. Properties
	// this code does not know about are kept as they are, as tools/vm.py does. One lock for the whole
	// process covers each read-change-write, so two callers never undo each other's change.
	public sealed class UserListWriter
	{
		public const string KeyAlphabet = "ABCDEFGHJKMNPQRSTVWXYZ23456789";   // no 0/O/1/I/L/U: friends may type it
		public const int KeyLength = 24;

		public const string Damaged = "The user list file is damaged; fix or restore it first.";
		public const string BadName = "That name can't be used. Use 1 to 24 letters, numbers, _ or -.";
		public const string BadKey = "That key isn't valid. A key is 24 characters: capital letters and the numbers 2 to 9, without I, L, O or U.";
		public const string NameTaken = "That name is taken.";
		public const string KeyTaken = "That key belongs to someone else.";
		public const string NoSuchUser = "There is no user with that name.";
		public const string LastCreator = "At least one user must be able to start games.";
		public const string NotSaved = "The user list could not be saved, so nothing was changed. Try again.";

		private static readonly object WriteLock = new();
		private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

		public string Path { get; }

		public UserListWriter(string path) => Path = path;

		public UserListWriter(IConfiguration config) : this(UserList.PathFor(config)) { }

		// The same rules as tools/vm.py valid_name / valid_key (and UserList.CleanName for names).
		public static bool ValidName(string? name) =>
			!string.IsNullOrEmpty(name) && name.Length <= UserList.MaxNameLength && UserList.CleanName(name) == name;

		public static bool ValidKey(string? key) =>
			key != null && key.Length == KeyLength && key.All(c => KeyAlphabet.Contains(c));

		public AdminRefusal? Read(out UserFileView? view)
		{
			view = null;
			lock (WriteLock)
			{
				if (!TryLoad(out JsonObject? root)) return new AdminRefusal(409, Damaged);
				view = ViewOf(root!);
				return null;
			}
		}

		public AdminRefusal? Add(string? name, string? key, bool canCreateGames, DateTime nowUtc, out StoredUser? added)
		{
			added = null;
			if (!ValidName(name)) return new AdminRefusal(400, BadName);
			if (!ValidKey(key)) return new AdminRefusal(400, BadKey);
			StoredUser? made = null;
			AdminRefusal? refusal = Change(users =>
			{
				if (Find(users, name!) != null) return new AdminRefusal(409, NameTaken);
				if (users.Any(u => KeyOf(u) == key)) return new AdminRefusal(409, KeyTaken);
				var user = new JsonObject
				{
					["Name"] = name,
					["Key"] = key,
					["CanCreateGames"] = canCreateGames,
					["ConfirmName"] = false,
					["Added"] = nowUtc.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture),
				};
				users.Add(user);
				made = UserOf(user);
				return null;
			});
			added = made;
			return refusal;
		}

		// A new name (the player must then join with it: ConfirmName goes off) and/or the
		// "can start games" setting. Both or neither are applied.
		public AdminRefusal? Update(string name, string? newName, bool? canCreateGames, out StoredUser? updated, out string? oldName)
		{
			updated = null;
			oldName = null;
			if (newName == null && canCreateGames == null) return new AdminRefusal(400, "Nothing to change: give a new name or the game-starting setting.");
			if (newName != null && !ValidName(newName)) return new AdminRefusal(400, BadName);
			StoredUser? result = null;
			string? was = null;
			AdminRefusal? refusal = Change(users =>
			{
				JsonObject? user = Find(users, name);
				if (user == null) return new AdminRefusal(404, NoSuchUser);
				if (newName != null)
				{
					JsonObject? other = Find(users, newName);
					if (other != null && other != user) return new AdminRefusal(409, NameTaken);
				}
				if (canCreateGames == false && CanCreate(user) && users.Count(CanCreate) == 1) return new AdminRefusal(409, LastCreator);
				was = NameOf(user);
				if (newName != null)
				{
					user["Name"] = newName;
					user["ConfirmName"] = false;
				}
				if (canCreateGames != null) user["CanCreateGames"] = canCreateGames.Value;
				result = UserOf(user);
				return null;
			});
			updated = result;
			oldName = was;
			return refusal;
		}

		public AdminRefusal? ReplaceKey(string name, string? key, out string? userName)
		{
			userName = null;
			if (!ValidKey(key)) return new AdminRefusal(400, BadKey);
			string? who = null;
			AdminRefusal? refusal = Change(users =>
			{
				JsonObject? user = Find(users, name);
				if (user == null) return new AdminRefusal(404, NoSuchUser);
				if (users.Any(u => u != user && KeyOf(u) == key)) return new AdminRefusal(409, KeyTaken);
				user["Key"] = key;
				who = NameOf(user);
				return null;
			});
			userName = who;
			return refusal;
		}

		public AdminRefusal? Remove(string name, out string? removedName)
		{
			string? gone = null;
			AdminRefusal? refusal = Change(users =>
			{
				JsonObject? user = Find(users, name);
				if (user == null) return new AdminRefusal(404, NoSuchUser);
				if (CanCreate(user) && users.Count(CanCreate) == 1) return new AdminRefusal(409, LastCreator);
				gone = NameOf(user);
				users.Remove(user);
				return null;
			});
			removedName = gone;
			return refusal;
		}

		public AdminRefusal? SetRequireKeys(bool on) => ChangeRoot(root =>
		{
			root["RequireKeys"] = on;
			return null;
		});

		// The user named `name` (any case), or null.
		public static StoredUser? FindIn(UserFileView view, string name) =>
			view.Users.FirstOrDefault(u => string.Equals(u.Name, name, StringComparison.OrdinalIgnoreCase));

		// --- reading and writing the file --------------------------------------------------------

		private AdminRefusal? Change(Func<JsonArray, AdminRefusal?> change) =>
			ChangeRoot(root => change(root["Users"]!.AsArray()));

		private AdminRefusal? ChangeRoot(Func<JsonObject, AdminRefusal?> change)
		{
			lock (WriteLock)
			{
				if (!TryLoad(out JsonObject? root)) return new AdminRefusal(409, Damaged);
				AdminRefusal? refusal = change(root!);
				if (refusal != null) return refusal;
				return Save(root!) ? null : new AdminRefusal(500, NotSaved);
			}
		}

		// False when the file is there but not a user list every reader accepts. A missing file is an
		// empty list with keys required (as tools/vm.py load_users).
		private bool TryLoad(out JsonObject? root)
		{
			root = null;
			if (!File.Exists(Path))
			{
				root = new JsonObject { ["RequireKeys"] = true, ["Users"] = new JsonArray() };
				return true;
			}
			try
			{
				byte[] bytes = File.ReadAllBytes(Path);
				ReadOnlySpan<byte> json = bytes;
				if (json.StartsWith(Encoding.UTF8.Preamble)) json = json[Encoding.UTF8.Preamble.Length..];
				if (JsonNode.Parse(json) is not JsonObject obj || !IsUserList(obj)) return false;
				root = obj;
				return true;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException)
			{
				return false;
			}
		}

		// The one strict shape (UserList.TryRead): RequireKeys true/false when present; Users a list of
		// objects with non-empty string Name and Key, CanCreateGames and ConfirmName true/false when present.
		private static bool IsUserList(JsonObject root)
		{
			if (!OptionalBool(root, "RequireKeys")) return false;
			if (!root.TryGetPropertyValue("Users", out JsonNode? users) || users is not JsonArray list) return false;
			foreach (JsonNode? item in list)
			{
				if (item is not JsonObject user) return false;
				if (string.IsNullOrEmpty(StringOf(user, "Name")) || string.IsNullOrEmpty(StringOf(user, "Key"))) return false;
				if (!OptionalBool(user, "CanCreateGames") || !OptionalBool(user, "ConfirmName")) return false;
			}
			return true;
		}

		private static bool OptionalBool(JsonObject obj, string name) =>
			!obj.TryGetPropertyValue(name, out JsonNode? v) || v?.GetValueKind() is JsonValueKind.True or JsonValueKind.False;

		private static string? StringOf(JsonObject obj, string name) =>
			obj.TryGetPropertyValue(name, out JsonNode? v) && v?.GetValueKind() == JsonValueKind.String ? v.GetValue<string>() : null;

		private static bool BoolOf(JsonObject obj, string name, bool fallback) =>
			obj.TryGetPropertyValue(name, out JsonNode? v) && v != null ? v.GetValue<bool>() : fallback;

		private static string NameOf(JsonNode? user) => StringOf((JsonObject)user!, "Name")!;
		private static string KeyOf(JsonNode? user) => StringOf((JsonObject)user!, "Key")!;
		private static bool CanCreate(JsonNode? user) => BoolOf((JsonObject)user!, "CanCreateGames", false);

		private static JsonObject? Find(JsonArray users, string name) =>
			users.Cast<JsonObject>().FirstOrDefault(u => string.Equals(NameOf(u), name, StringComparison.OrdinalIgnoreCase));

		private static StoredUser UserOf(JsonObject user)
		{
			DateTime? added = DateTime.TryParse(StringOf(user, "Added"), CultureInfo.InvariantCulture,
				DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out DateTime d) ? d : null;
			return new StoredUser(NameOf(user), KeyOf(user), BoolOf(user, "CanCreateGames", false), BoolOf(user, "ConfirmName", false), added);
		}

		private static UserFileView ViewOf(JsonObject root) =>
			new(BoolOf(root, "RequireKeys", true), root["Users"]!.AsArray().Select(u => UserOf((JsonObject)u!)).ToList());

		// The whole file to a .tmp beside it, then one rename over the old one, so the game server and
		// matchmaking (which re-read it on change) never see half a list. Indented by two with a final
		// newline, like tools/vm.py save_users.
		private bool Save(JsonObject root)
		{
			string tmp = Path + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				string? dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(Path));
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				File.WriteAllText(tmp, root.ToJsonString(Indented) + Environment.NewLine, new UTF8Encoding(false));
				for (int attempt = 1; ; attempt++)
				{
					try
					{
						File.Move(tmp, Path, overwrite: true);
						return true;
					}
					catch (Exception e) when (attempt < 5 && e is IOException or UnauthorizedAccessException)
					{
						Thread.Sleep(50);   // a reader or a virus scanner had it open for a moment
					}
				}
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return false;
			}
			finally
			{
				try { File.Delete(tmp); } catch { }
			}
		}
	}
}
