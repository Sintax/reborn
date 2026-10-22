using System.IO.Compression;
using System.Text;
using System.Text.Json;
using ControlPanel.Core;

namespace ControlPanel.Tests
{
	public class LogicTests
	{
		[Fact]
		public void Keys_are_valid_and_differ()
		{
			var keys = Enumerable.Range(0, 200).Select(_ => Keys.New()).ToList();
			Assert.All(keys, k =>
			{
				Assert.Equal(24, k.Length);
				Assert.All(k, c => Assert.Contains(c, "ABCDEFGHJKMNPQRSTVWXYZ23456789"));
				Assert.True(Keys.ValidKey(k));
			});
			Assert.Equal(keys.Count, keys.Distinct().Count());

			Assert.False(Keys.ValidKey(""));
			Assert.False(Keys.ValidKey(null));
			Assert.False(Keys.ValidKey("ABCDEFGHJKMNPQRSTVWXYZ2"));     // 23 characters
			Assert.False(Keys.ValidKey("ABCDEFGHJKMNPQRSTVWXYZ234"));   // 25
			Assert.False(Keys.ValidKey("ABCDEFGHJKMNPQRSTVWXYZ2O"));    // O is not in the alphabet
			Assert.False(Keys.ValidKey("ABCDEFGHJKMNPQRSTVWXYZ2u"));    // lower case
			Assert.False(Keys.ValidKey("ABCDEFGHJKMNPQRSTVWXYZ21"));    // 1 is not in the alphabet
		}

		[Fact]
		public void Name_rules_match_the_server()
		{
			// tools/vm.py valid_name: [A-Za-z0-9_-]{1,24}; UserListWriter.ValidName: CleanName round-trip, 1-24.
			foreach (string good in new[] { "Kaz", "a", "Test-c1", "Sintax_99", "ABCDEFGHIJKLMNOPQRSTUVWX", "-", "_" })
				Assert.True(Keys.ValidName(good), good);
			foreach (string? bad in new[] { null, "", " ", "Kaz ", " Kaz", "Ka z", "Kaz!", "Käz", "ABCDEFGHIJKLMNOPQRSTUVWXY", "Kaz.", "Kaz\n", "a/b", "名前" })
				Assert.False(Keys.ValidName(bad), bad ?? "(null)");
		}

		private static string MakeBaseZip(string dir, bool withUserIni = true)
		{
			string path = Path.Combine(dir, "reborn-player-installer.zip");
			using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
			void Put(string name, string text)
			{
				using var w = new StreamWriter(zip.CreateEntry(name).Open());
				w.Write(text);
			}
			Put("Install.bat", "@echo off\r\n");
			Put("Install.ps1", "# install\r\n");
			Put("README.txt", "Battleborn Reborn\r\n");
			Put("files/reborn.dll", "dll bytes");
			Put("files/steam_settings/configs.main.ini", "[main::connectivity]\r\n");
			if (withUserIni) Put("files/steam_settings/configs.user.ini", "[user::general]\r\naccount_name=Someone\r\n");
			Put("files/reborn-key.txt", "OLDKEYOLDKEYOLDKEYOLDKEY\r\n");
			return path;
		}

		private static string Text(ZipArchive zip, string name)
		{
			using var r = new StreamReader(zip.GetEntry(name)!.Open());
			return r.ReadToEnd();
		}

		[Fact]
		public void Friend_zip_has_the_three_files_and_the_right_steam_id()
		{
			using var t = new TempPanel();
			string baseZip = MakeBaseZip(t.Root);
			string outDir = Path.Combine(t.Root, "out");
			string record = Path.Combine(t.Root, "reborn-zips.json");
			string key = "ABCDEFGHJKMNPQRSTVWXYZ23";

			// python -c "from tools import package; print(package.friend_steamid('Kaz'))"
			Assert.Equal(76561197960325377UL, FriendZip.SteamId("Kaz"));

			var (ok, message, path) = FriendZip.Make(baseZip, "Kaz", key, outDir, record, new DateTime(2026, 10, 22, 12, 0, 0, DateTimeKind.Utc));
			Assert.True(ok, message);
			Assert.Equal(Path.Combine(outDir, "Reborn-Mod-Installer_Kaz.zip"), path);
			Assert.DoesNotContain(key, message);
			Assert.Empty(Directory.GetFiles(outDir, "*.tmp"));

			using (var zip = ZipFile.OpenRead(path!))
			{
				var names = zip.Entries.Select(e => e.FullName).ToList();
				// every base entry once, plus nothing else; same-named entries were replaced, not doubled
				Assert.Equal(new[] { "Install.bat", "Install.ps1", "README.txt", "files/reborn.dll", "files/steam_settings/configs.main.ini",
					"files/steam_settings/configs.user.ini", "files/reborn-key.txt", "files/reborn-player.txt" }.OrderBy(n => n),
					names.OrderBy(n => n));
				Assert.Equal("[user::general]\r\naccount_name=Kaz\r\naccount_steamid=76561197960325377\r\nlanguage=english\r\n",
					Text(zip, "files/steam_settings/configs.user.ini"));
				Assert.Equal(key + "\r\n", Text(zip, "files/reborn-key.txt"));
				Assert.Equal("Kaz\r\n", Text(zip, "files/reborn-player.txt"));
				Assert.Equal("dll bytes", Text(zip, "files/reborn.dll"));
			}

			using (var doc = JsonDocument.Parse(File.ReadAllText(record)))
				Assert.Equal("2026-10-22T12:00:00Z", doc.RootElement.GetProperty("Kaz").GetString());
			Assert.Equal(new DateTime(2026, 10, 22, 12, 0, 0, DateTimeKind.Utc), ZipRecords.Load(record)["kaz"]);

			// A second zip (another name, base without a user ini) keeps the first record.
			string baseZip2 = MakeBaseZip(Directory.CreateDirectory(Path.Combine(t.Root, "second")).FullName, withUserIni: false);
			var second = FriendZip.Make(baseZip2, "Pip", "BCDEFGHJKMNPQRSTVWXYZ234", outDir, record, DateTime.UtcNow);
			Assert.True(second.ok, second.message);
			Assert.Equal(2, ZipRecords.Load(record).Count);
			using (var zip = ZipFile.OpenRead(second.path!))
				Assert.Equal("Pip\r\n", Text(zip, "files/reborn-player.txt"));
		}

		[Fact]
		public void Missing_or_bad_base_zip_gives_a_message_and_no_file()
		{
			using var t = new TempPanel();
			string outDir = Path.Combine(t.Root, "out");
			string record = Path.Combine(t.Root, "reborn-zips.json");
			string key = "ABCDEFGHJKMNPQRSTVWXYZ23";

			void Refused(string baseZip, string name, string k, string expect)
			{
				var (ok, message, path) = FriendZip.Make(baseZip, name, k, outDir, record, DateTime.UtcNow);
				Assert.False(ok);
				Assert.Null(path);
				Assert.Contains(expect, message);
				Assert.DoesNotContain(k, message);
				Assert.True(!Directory.Exists(outDir) || Directory.GetFiles(outDir).Length == 0, "no file may be left behind");
				Assert.False(File.Exists(record));
			}

			Refused(Path.Combine(t.Root, "missing.zip"), "Kaz", key, "player installer zip is missing");

			string notZip = Path.Combine(t.Root, "not-a-zip.zip");
			File.WriteAllText(notZip, "this is not a zip file at all");
			Refused(notZip, "Kaz", key, "damaged");

			// A real zip whose stored data is broken part-way: found while copying, after the .tmp was started.
			string good = MakeBaseZip(t.Root);
			byte[] bytes = File.ReadAllBytes(good);
			byte[] needle = Encoding.ASCII.GetBytes("files/reborn.dll");
			int at = IndexOf(bytes, needle, 0);                          // the local header's name
			Assert.True(at > 0);
			for (int i = at + needle.Length; i < at + needle.Length + 4; i++) bytes[i] ^= 0xFF;   // break its data
			string broken = Path.Combine(t.Root, "broken.zip");
			File.WriteAllBytes(broken, bytes);
			Refused(broken, "Kaz", key, "damaged");

			string empty = Path.Combine(t.Root, "empty.zip");
			using (ZipFile.Open(empty, ZipArchiveMode.Create)) { }
			Refused(empty, "Kaz", key, "does not look like the player installer");

			Refused(good, "Kaz!", key, "name");
			Refused(good, "Kaz", "not a key", "key");
		}

		private static int IndexOf(byte[] hay, byte[] needle, int from)
		{
			for (int i = from; i <= hay.Length - needle.Length; i++)
				if (hay.AsSpan(i, needle.Length).SequenceEqual(needle)) return i;
			return -1;
		}

		[Fact]
		public void Memory_plan_text_and_too_many()
		{
			var plan = new MemoryPlan(6.0, 2.1, 3);
			Assert.Equal("Each game uses about 1.3 GB. 3 games need about 3.9 GB; this server has 6.0 GB (2.1 GB free).", plan.Summary);
			Assert.False(plan.TooMany);                              // 3.9 <= 5.0

			Assert.True(new MemoryPlan(6.0, 2.1, 4).TooMany);         // 5.2 > 5.0
			Assert.False(new MemoryPlan(6.0, 2.1, 3).TooMany);
			Assert.Equal("Each game uses about 1.3 GB. 1 game needs about 1.3 GB; this server has 16.0 GB (9.5 GB free).",
				new MemoryPlan(16.0, 9.5, 1).Summary);
			Assert.Contains("more games than", new MemoryPlan(4.0, 1.0, 3).Warning);
			Assert.Equal("", new MemoryPlan(16.0, 9.5, 3).Warning);
		}

		[Fact]
		public void Firewall_args_match_the_bat_file()
		{
			string exe = @"C:\Games\Battleborn\Binaries\Win64\Serverborn.exe";
			string[] args = Firewall.ArgsFor(new[] { 7777, 7778, 7780 }, exe);
			Assert.Equal(2, args.Length);

			// The same two commands as tools/serverkit_files/OpenFirewall.bat, with the panel's port list.
			string bat = File.ReadAllText(Path.Combine(RepoRoot(), "tools", "serverkit_files", "OpenFirewall.bat"));
			string Line(string start) => bat.Split('\n').Select(l => l.Trim()).Single(l => l.StartsWith(start));
			string delete = Line("netsh advfirewall firewall delete rule name=\"Battleborn Reborn server\"");
			string add = Line("netsh advfirewall firewall add rule name=\"Battleborn Reborn server\"");

			Assert.Equal(delete.Replace(" >nul 2>&1", "")["netsh ".Length..], args[0]);
			Assert.Equal(add.Replace("%~dp0Serverborn.exe", exe).Replace("localport=7777-7779", "localport=7777,7778,7780")["netsh ".Length..], args[1]);
			Assert.Equal("advfirewall firewall add rule name=\"Battleborn Reborn server\" dir=in action=allow profile=any program=\""
				+ exe + "\" protocol=UDP localport=7777,7778,7780 edge=no", args[1]);
		}

		[Fact]
		public async Task Client_reports_matchmaking_down_without_throwing()
		{
			using var t = new TempPanel();
			t.Api.Down = true;
			AdminClient client = t.Client();

			var status = await client.GetStatusAsync();
			Assert.False(status.Ok);
			Assert.True(status.NotRunning);
			Assert.Equal("Matchmaking is not running.", status.Message);
			Assert.Null(status.Value);
			Assert.False((await client.RemoveUserAsync("Kaz")).Ok);
			Assert.Equal("Matchmaking is not running.", (await client.SetPortsAsync(new[] { 7777 })).Message);

			// Up again: the token goes with every call, and the API's own words come back as they are.
			t.Api.Down = false;
			status = await client.GetStatusAsync();
			Assert.True(status.Ok, status.Message);
			Assert.Equal("203.0.113.10", status.Value!.PublicIp);
			Assert.Equal(new[] { 7777, 7778 }, status.Value.GamePortsInUse);
			Assert.Equal("TOKENTOKENTOKENTOKENTOKENTOKEN22", t.Api.Requests.Last().Token);
			Assert.Equal("/api/admin/status", t.Api.Requests.Last().Path);

			t.Api.Answer = s => FakeApi.Message(409, "That name is taken.");
			var add = await client.AddUserAsync("Kaz", Keys.New(), false);
			Assert.False(add.Ok);
			Assert.False(add.NotRunning);
			Assert.Equal("That name is taken.", add.Message);

			// A refused token: a plain message, never the token.
			t.Api.Answer = s => new HttpResponseMessage(System.Net.HttpStatusCode.Forbidden);
			var refused = await client.GetUsersAsync();
			Assert.False(refused.Ok);
			Assert.Equal("The Control Panel could not sign in to matchmaking. Restart matchmaking.", refused.Message);
			Assert.DoesNotContain("TOKEN", refused.Message);

			// Something unreadable does not throw either.
			t.Api.Answer = s => new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new StringContent("<html>") };
			var odd = await client.GetGamesAsync();
			Assert.False(odd.Ok);
			Assert.False(string.IsNullOrEmpty(odd.Message));

			// Names in the address are escaped.
			t.Api.Answer = null;
			await client.GetKeyAsync("a/b c");
			Assert.Equal("/api/admin/users/a%2Fb%20c/key", t.Api.Requests.Last().Path);
		}

		[Fact]
		public void Paths_find_the_game_folder_and_the_matchmaking_port()
		{
			using var t = new TempPanel("{ \"CoordinatorPort\": \"5123\", /* note */ \"GamePorts\": [7777], }");
			Assert.Equal(t.Win64, t.Paths.Win64);                                     // exe in Win64\ControlPanel
			Assert.Equal(t.Win64, new PanelPaths(t.Win64, t.Profile).Win64);           // exe right in Win64
			Assert.Equal(Path.Combine(t.Win64, "matchmaking"), t.Paths.MatchmakingDir);
			Assert.Equal(Path.Combine(t.Win64, "matchmaking", "admin-token.txt"), t.Paths.AdminTokenFile);
			Assert.Equal(Path.Combine(t.Win64, "matchmaking", "matchmaking.log"), t.Paths.MatchmakingLog);
			Assert.Equal(Path.Combine(t.Win64, "reborn-player-installer.zip"), t.Paths.PlayerInstallerZip);
			Assert.Equal(Path.Combine(t.Win64, "reborn-zips.json"), t.Paths.ZipRecordFile);
			Assert.Equal(Path.Combine(t.Profile, "Documents", "Reborn Friend Zips"), t.Paths.ZipOutputDir);
			Assert.Equal(Path.Combine(t.Profile, "Documents", "RebornLogs"), t.Paths.GameLogsDir);
			Assert.Equal(Path.Combine(t.Win64, "Serverborn.exe"), t.Paths.ServerExe);
			Assert.Equal(5123, t.Paths.CoordinatorPort);
			Assert.Equal(new Uri("http://127.0.0.1:5123/api/admin/"), t.Paths.AdminBaseUri);
			Assert.Equal("TOKENTOKENTOKENTOKENTOKENTOKEN22", t.Paths.ReadToken());

			File.WriteAllText(Path.Combine(t.Win64, "matchmaking", "appsettings.json"), "{ broken");
			Assert.Equal(5000, t.Paths.CoordinatorPort);
			File.WriteAllText(Path.Combine(t.Win64, "matchmaking", "appsettings.json"), "{ \"CoordinatorPort\": 5200 }");
			Assert.Equal(5200, t.Paths.CoordinatorPort);
			File.Delete(t.Paths.AdminTokenFile);
			Assert.Null(t.Paths.ReadToken());
		}

		internal static string RepoRoot()
		{
			for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
				if (File.Exists(Path.Combine(dir.FullName, "reborn.sln"))) return dir.FullName;
			throw new InvalidOperationException("reborn.sln not found above the test folder");
		}
	}
}
