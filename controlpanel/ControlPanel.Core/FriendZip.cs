using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace ControlPanel.Core
{
	// One friend's zip: the player installer zip that came with the server update, plus that friend's
	// own Steam identity, key and player name (the same files tools/package.py make_friend_zip adds).
	public static class FriendZip
	{
		public const ulong SteamIdBase = 76561197960287930UL;   // tools/launch.py STEAMID_BASE

		// The same as tools/package.py friend_steamid: base + CRC-32("friend:" + name) % 100000.
		public static ulong SteamId(string name) => SteamIdBase + Crc32.Of(Encoding.UTF8.GetBytes("friend:" + name)) % 100000;

		public static string FileName(string name) => $"Reborn-Mod-Installer_{name}.zip";

		public static string ReadmeName(string name) => $"README-{name}.txt";

		// The friend's guide, as tools/package.py friend_readme makes it: docs/FRIEND-SETUP.md (built
		// into this program) with {NAME} filled in, CRLF line ends and the version on top.
		public static string Readme(string name, string version)
		{
			using Stream stream = typeof(FriendZip).Assembly.GetManifestResourceStream("FRIEND-SETUP.md")
				?? throw new InvalidOperationException("FRIEND-SETUP.md is missing from the program");
			using var reader = new StreamReader(stream, new UTF8Encoding(false), detectEncodingFromByteOrderMarks: false);
			string doc = reader.ReadToEnd().Replace("\r\n", "\n").Replace("{NAME}", name);   // Python reads it with \n line ends
			return $"Battleborn Reborn {version}\r\n\r\n" + doc.Replace("\n", "\r\n");
		}

		// Writes <outDir>\Reborn-Mod-Installer_<name>.zip through a .tmp file, so a failure never
		// leaves a half-written zip, then records the date in recordFile (when given). version goes on
		// top of the guide (the server's version, as matchmaking reports it). The message is plain
		// text for the owner and never contains the key.
		public static (bool ok, string message, string? path) Make(string baseZip, string name, string key, string outDir,
			string? recordFile = null, DateTime? nowUtc = null, string version = "")
		{
			if (!Keys.ValidName(name)) return (false, Keys.BadName, null);
			if (!Keys.ValidKey(key)) return (false, "That user's key isn't valid. Give them a new key first.", null);
			if (!File.Exists(baseZip))
				return (false, $"The player installer zip is missing ({baseZip}). It comes with each server update; install the update again.", null);

			var added = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
			{
				["files/steam_settings/configs.user.ini"] =
					$"[user::general]\r\naccount_name={name}\r\naccount_steamid={SteamId(name)}\r\nlanguage=english\r\n",
				["files/reborn-key.txt"] = key + "\r\n",
				["files/reborn-player.txt"] = name + "\r\n",
				[ReadmeName(name)] = Readme(name, version),
			};

			string path = Path.Combine(outDir, FileName(name));
			string tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				Directory.CreateDirectory(outDir);
				using (ZipArchive source = ZipFile.OpenRead(baseZip))
				{
					if (!LooksLikeInstaller(source))
						return (false, "The player installer zip does not look like the player installer. Install the server update again.", null);
					using var output = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write);
					using var zip = new ZipArchive(output, ZipArchiveMode.Create);
					foreach (ZipArchiveEntry entry in source.Entries)
					{
						if (added.ContainsKey(Normal(entry.FullName))) continue;   // replaced below
						ZipArchiveEntry copy = zip.CreateEntry(entry.FullName, CompressionLevel.Optimal);
						copy.LastWriteTime = entry.LastWriteTime;
						using Stream from = entry.Open();
						using Stream to = copy.Open();
						from.CopyTo(to);
					}
					foreach (var (entryName, text) in added)
					{
						using var w = new StreamWriter(zip.CreateEntry(entryName, CompressionLevel.Optimal).Open(), new UTF8Encoding(false));
						w.Write(text);
					}
				}
				File.Move(tmp, path, overwrite: true);
			}
			catch (Exception e) when (e is InvalidDataException or EndOfStreamException)
			{
				return (false, "The player installer zip is damaged. Install the server update again.", null);
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return (false, $"The zip could not be written in {outDir}. Check that the folder can be written to and the zip is not open, then try again.", null);
			}
			finally
			{
				Delete(tmp);
			}

			if (recordFile != null) ZipRecords.Record(recordFile, name, nowUtc ?? DateTime.UtcNow);
			return (true, $"{name}'s zip is ready: {path}. Send it to {name} only: it holds their key.", path);
		}

		private static string Normal(string entryName) => entryName.Replace('\\', '/');

		// The installer has Install.bat on top and the game files under files/.
		private static bool LooksLikeInstaller(ZipArchive zip) =>
			zip.Entries.Any(e => Normal(e.FullName).Equals("Install.bat", StringComparison.OrdinalIgnoreCase))
			&& zip.Entries.Any(e => Normal(e.FullName).StartsWith("files/", StringComparison.OrdinalIgnoreCase));

		private static void Delete(string file)
		{
			try { if (File.Exists(file)) File.Delete(file); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
		}
	}

	// reborn-zips.json: when each user's zip was last made, as { "Kaz": "2026-10-22T12:00:00Z" }.
	// Written through a .tmp file. A missing or damaged file reads as empty.
	public static class ZipRecords
	{
		private static readonly object Lock = new();

		public static Dictionary<string, DateTime> Load(string file)
		{
			var made = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);
			foreach (var (name, when) in Read(file))
				if (when is JsonValue v && v.TryGetValue(out string? text)
					&& DateTime.TryParse(text, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out DateTime utc))
					made[name] = DateTime.SpecifyKind(utc, DateTimeKind.Utc);
			return made;
		}

		public static void Record(string file, string name, DateTime whenUtc) => Change(file, root =>
		{
			Remove(root, name);
			root[name] = whenUtc.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
		});

		// The user's zip is out of date (new key, new name) or they were removed.
		public static void Forget(string file, string name) => Change(file, root => Remove(root, name));

		private static void Remove(JsonObject root, string name)
		{
			foreach (string old in root.Select(p => p.Key).Where(k => string.Equals(k, name, StringComparison.OrdinalIgnoreCase)).ToList())
				root.Remove(old);
		}

		private static JsonObject Read(string file)
		{
			try
			{
				if (File.Exists(file) && JsonNode.Parse(File.ReadAllText(file)) is JsonObject root) return root;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
			return new JsonObject();
		}

		private static void Change(string file, Action<JsonObject> change)
		{
			lock (Lock)
			{
				JsonObject root = Read(file);
				change(root);
				string tmp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
				try
				{
					string? dir = Path.GetDirectoryName(Path.GetFullPath(file));
					if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
					File.WriteAllText(tmp, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
					File.Move(tmp, file, overwrite: true);
				}
				catch (Exception e) when (e is IOException or UnauthorizedAccessException)
				{
					// Only the "zip made" column is affected; the zip itself is fine.
					try { if (File.Exists(tmp)) File.Delete(tmp); } catch (Exception) { }
				}
			}
		}
	}

	// CRC-32 (IEEE, as zlib.crc32).
	public static class Crc32
	{
		private static readonly uint[] Table = MakeTable();

		private static uint[] MakeTable()
		{
			var table = new uint[256];
			for (uint i = 0; i < 256; i++)
			{
				uint c = i;
				for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				table[i] = c;
			}
			return table;
		}

		public static uint Of(ReadOnlySpan<byte> data)
		{
			uint crc = 0xFFFFFFFFu;
			foreach (byte b in data) crc = Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
			return crc ^ 0xFFFFFFFFu;
		}
	}
}
