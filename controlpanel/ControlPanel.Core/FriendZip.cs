using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace ControlPanel.Core
{
	// One friend's zip: the player installer zip that came with the server update (built with the Steam
	// loader, as tools/vm.py release makes it), plus that friend's own Steam identity, key, player name
	// and guide. The result holds the same files as tools/package.py make_friend_zip: the general guide
	// (README.txt) is left out, and README-<name>.txt takes its place.
	public static class FriendZip
	{
		public const ulong SteamIdBase = 76561197960287930UL;   // tools/launch.py STEAMID_BASE

		// The same as tools/package.py friend_steamid: base + CRC-32("friend:" + name) % 100000.
		public static ulong SteamId(string name) => SteamIdBase + Crc32.Of(Encoding.UTF8.GetBytes("friend:" + name)) % 100000;

		public static string FileName(string name) => $"Reborn-Mod-Installer_{name}.exe";

		public static string ReadmeName(string name) => $"README-{name}.txt";

		// The standard installer's general guide; a friend's zip has their own guide instead.
		public const string BaseReadmeName = "README.txt";

		// The friend's guide, as tools/package.py friend_readme makes it: docs/FRIEND-SETUP.md (built
		// into this program) with {NAME} filled in, CRLF line ends and the version on top.
		public static string Readme(string name, string version)
		{
			using Stream stream = typeof(FriendZip).Assembly.GetManifestResourceStream("FRIEND-SETUP.md")
				?? throw new InvalidOperationException("FRIEND-SETUP.md is missing from the program");
			using var reader = new StreamReader(stream, new UTF8Encoding(false), detectEncodingFromByteOrderMarks: false);
			string doc = reader.ReadToEnd().Replace("\r\n", "\n").Replace("{NAME}", name);   // Python reads it with \n line ends
			string shown = string.IsNullOrWhiteSpace(version) ? "unknown" : version;   // an older matchmaking may not say
			return $"Battleborn Reborn {shown}\r\n\r\n" + doc.Replace("\n", "\r\n");
		}

		// Packs the friend's files into <outDir>\Reborn-Mod-Installer_<name>.exe: a self-unpacking 7-Zip file
		// (no password). The files
		// are first laid out in a staging folder under outDir, which is deleted on every path. The file is
		// built under a temp name and moved over the old one only when 7-Zip succeeded. Then the date is
		// recorded in recordFile (when given). version goes on top of the guide (the server's version, as
		// matchmaking reports it). The message is plain text for the owner and never contains the key. sevenZipDir: the folder with 7z.exe and 7z.sfx (null: look for 7-Zip with findSevenZip, or the usual places).
		public static (bool ok, string message, string? path) Make(string baseZip, string name, string key, string outDir,
			IProcessRunner runner, string? recordFile = null, DateTime? nowUtc = null, string version = "", string? sevenZipDir = null,
			TimeSpan? limit = null, Func<string?>? findSevenZip = null)
		{
			if (!Keys.ValidName(name)) return (false, Keys.BadName, null);
			if (!Keys.ValidKey(key)) return (false, "That user's key isn't valid. Give them a new key first.", null);
			if (!File.Exists(baseZip))
				return (false, $"The player installer zip is missing ({baseZip}). It comes with each server update; install the update again.", null);
			string? sevenZip = sevenZipDir ?? (findSevenZip ?? (() => SevenZip.Find()))();
			if (sevenZip == null) return (false, SevenZip.NotInstalled, null);

			var added = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
			{
				["files/steam_settings/configs.user.ini"] =
					$"[user::general]\r\naccount_name={name}\r\naccount_steamid={SteamId(name)}\r\nlanguage=english\r\n",
				["files/reborn-key.txt"] = key + "\r\n",
				["files/reborn-player.txt"] = name + "\r\n",
				[ReadmeName(name)] = Readme(name, version),
			};

			string path = Path.Combine(outDir, FileName(name));
			string staging = Path.Combine(outDir, ".staging-" + Guid.NewGuid().ToString("N"));
			try
			{
				Directory.CreateDirectory(outDir);
				using (ZipArchive source = ZipFile.OpenRead(baseZip))
				{
					if (!LooksLikeInstaller(source))
						return (false, "The player installer zip does not look like the player installer. Install the server update again.", null);
					Directory.CreateDirectory(staging);
					foreach (ZipArchiveEntry entry in source.Entries)
					{
						string entryName = Normal(entry.FullName);
						if (added.ContainsKey(entryName)) continue;   // replaced below
						if (entryName.Equals(BaseReadmeName, StringComparison.OrdinalIgnoreCase)) continue;   // their own guide instead
						if (entryName.EndsWith('/')) continue;   // a folder entry; its files make the folder
						string target = Inside(staging, entryName);
						Directory.CreateDirectory(Path.GetDirectoryName(target)!);
						entry.ExtractToFile(target, overwrite: false);
						File.SetLastWriteTime(target, entry.LastWriteTime.LocalDateTime);
					}
				}
				foreach (var (entryName, text) in added)
				{
					string target = Inside(staging, entryName);
					Directory.CreateDirectory(Path.GetDirectoryName(target)!);
					File.WriteAllText(target, text, new UTF8Encoding(false));
				}

				var (packed, packMessage) = SevenZip.PackSfx(sevenZip, staging, path, runner, limit ?? SevenZip.PackLimit);
				if (!packed) return (false, packMessage, null);

				if (recordFile != null) ZipRecords.Record(recordFile, name, nowUtc ?? DateTime.UtcNow);
				return (true, $"{name}'s file is ready: {path}. Send it to {name} only: it holds their key.", path);
			}
			catch (Exception e) when (e is InvalidDataException or EndOfStreamException)
			{
				return (false, "The player installer zip is damaged. Install the server update again.", null);
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return (false, $"The file could not be made in {outDir}. Check that the folder can be written to and the file is not open, then try again.", null);
			}
			finally
			{
				DeleteFolder(staging);
			}
		}

		// A path inside root, refusing a name that would climb out of it.
		private static string Inside(string root, string relative)
		{
			string full = Path.GetFullPath(Path.Combine(root, relative));
			if (!full.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
				throw new InvalidDataException("An entry leaves its folder");
			return full;
		}

		private static void DeleteFolder(string dir)
		{
			try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
		}

		private static string Normal(string entryName) => entryName.Replace('\\', '/');

		// The installer has Install.bat on top and the game files under files/.
		private static bool LooksLikeInstaller(ZipArchive zip) =>
			zip.Entries.Any(e => Normal(e.FullName).Equals("Install.bat", StringComparison.OrdinalIgnoreCase))
			&& zip.Entries.Any(e => Normal(e.FullName).StartsWith("files/", StringComparison.OrdinalIgnoreCase));
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
