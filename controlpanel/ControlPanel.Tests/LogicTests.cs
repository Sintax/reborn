using System.IO.Compression;
using System.Text;
using System.Text.Json;
using ControlPanel.Core;
using ControlPanel.Core.ViewModels;

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

			var (ok, message, path) = FriendZip.Make(baseZip, "Kaz", key, outDir, record, new DateTime(2026, 10, 22, 12, 0, 0, DateTimeKind.Utc), "1.0.7");
			Assert.True(ok, message);
			Assert.Equal(Path.Combine(outDir, "Reborn-Mod-Installer_Kaz.zip"), path);
			Assert.DoesNotContain(key, message);
			Assert.Empty(Directory.GetFiles(outDir, "*.tmp"));

			// The friend's guide, made as tools/package.py friend_readme does: docs/FRIEND-SETUP.md with
			// {NAME} filled in, CRLF line ends, a "Battleborn Reborn <version>" line on top, UTF-8 without BOM.
			string guideText = File.ReadAllText(Path.Combine(RepoRoot(), "docs", "FRIEND-SETUP.md")).Replace("\r\n", "\n");
			byte[] guide = new UTF8Encoding(false).GetBytes("Battleborn Reborn 1.0.7\r\n\r\n" + guideText.Replace("{NAME}", "Kaz").Replace("\n", "\r\n"));

			using (var zip = ZipFile.OpenRead(path!))
			{
				var names = zip.Entries.Select(e => e.FullName).ToList();
				// every base entry once, plus nothing else; same-named entries were replaced, not doubled;
				// the general guide (README.txt) gives way to the friend's own, as in tools/package.py
				Assert.Equal(new[] { "Install.bat", "Install.ps1", "files/reborn.dll", "files/steam_settings/configs.main.ini",
					"files/steam_settings/configs.user.ini", "files/reborn-key.txt", "files/reborn-player.txt", "README-Kaz.txt" }.OrderBy(n => n),
					names.OrderBy(n => n));
				using (var s = zip.GetEntry("README-Kaz.txt")!.Open())
				using (var m = new MemoryStream())
				{
					s.CopyTo(m);
					Assert.Equal(guide, m.ToArray());
				}
				Assert.Contains("# Battleborn Reborn: setup for Kaz\r\n", Text(zip, "README-Kaz.txt"));
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
			{
				Assert.Equal("Pip\r\n", Text(zip, "files/reborn-player.txt"));
				// No server version known (an older matchmaking): the guide says so instead of a blank.
				Assert.StartsWith("Battleborn Reborn unknown\r\n\r\n", Text(zip, "README-Pip.txt"));
			}
		}

		// tools/package.py, given the same files, makes both the base zip (`make_zip` with the Steam
		// loader: what `tools.vm release` uploads as reborn-player-installer.zip) and a friend zip
		// (`make_friend_zip`, today's `--friend` zip). The panel's zip made from that base must hold the
		// same entries with the same bytes.
		private const string MakeBothZips = """
			import sys
			from pathlib import Path
			from tools import launch, package
			work, key = Path(sys.argv[1]), sys.argv[2]
			win64 = work / "Win64"
			win64.mkdir(parents=True)
			mod, dxgi = work / "reborn.dll", work / "dxgi.dll"
			mod.write_bytes(b"mod bytes")
			dxgi.write_bytes(b"dxgi bytes")
			for f in launch.LOADER_FILES:
			    (win64 / f).write_bytes(f.encode())
			(win64 / package.LOADER_INI).write_text("[SteamClient]\nExe=Battleborn.exe\nExeCommandLine=\n", encoding="utf-8")
			package.make_zip(work / "base.zip", "abc1234", True, win64, mod, dxgi)
			package.make_friend_zip(work / "friend.zip", "abc1234", "Kaz", key, win64, mod, dxgi)
			""";

		[Fact]
		public void Friend_zip_matches_the_one_tools_package_makes()
		{
			using var t = new TempPanel();
			string work = Path.Combine(t.Root, "py");
			string key = "ABCDEFGHJKMNPQRSTVWXYZ23";
			var start = new System.Diagnostics.ProcessStartInfo("python")
			{
				WorkingDirectory = RepoRoot(),
				RedirectStandardOutput = true,
				RedirectStandardError = true,
				UseShellExecute = false,
			};
			foreach (string arg in new[] { "-c", MakeBothZips, work, key }) start.ArgumentList.Add(arg);
			using (var python = System.Diagnostics.Process.Start(start)!)
			{
				string output = python.StandardOutput.ReadToEnd() + python.StandardError.ReadToEnd();
				Assert.True(python.WaitForExit(60_000), "python did not finish");
				Assert.True(python.ExitCode == 0, "tools/package.py could not make the zips:\n" + output);
			}

			var (ok, message, path) = FriendZip.Make(Path.Combine(work, "base.zip"), "Kaz", key, Path.Combine(t.Root, "out"), version: "abc1234");
			Assert.True(ok, message);

			static SortedDictionary<string, byte[]> Entries(string zipPath)
			{
				var all = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
				using var zip = ZipFile.OpenRead(zipPath);
				foreach (ZipArchiveEntry e in zip.Entries)
				{
					using var s = e.Open();
					using var m = new MemoryStream();
					s.CopyTo(m);
					all.Add(e.FullName, m.ToArray());
				}
				return all;
			}

			var expected = Entries(Path.Combine(work, "friend.zip"));
			var made = Entries(path!);
			Assert.Equal(expected.Keys, made.Keys);
			foreach (string name in expected.Keys)
				Assert.True(expected[name].AsSpan().SequenceEqual(made[name]), name + " differs from tools/package.py's");

			// The Steam loader is in, so Install.ps1 makes the desktop shortcut and never asks the friend
			// to get loader files from the host.
			Assert.Contains("files/steamclient_loader_x64.exe", made.Keys);
			Assert.Contains("files/ColdClientLoader.ini", made.Keys);
			Assert.DoesNotContain("README.txt", made.Keys);
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

			// Both in one run as administrator (one Windows question): cmd /s strips only the outer quotes,
			// so the quoted rule name and program path reach netsh as they are; & runs the add after the delete.
			string spaced = @"C:\Program Files (x86)\Steam\steamapps\common\Battleborn\Binaries\Win64\Serverborn.exe";
			string[] spacedArgs = Firewall.ArgsFor(new[] { 7777 }, spaced);
			// /v:off: delayed expansion stays off, so a ! in the game's path is passed on as it is.
			Assert.Equal("/d /s /v:off /c \"netsh " + spacedArgs[0] + " & netsh " + spacedArgs[1] + "\"", Firewall.CommandLine(new[] { 7777 }, spaced));
			Assert.Contains(@"program=""C:\Games\Wow!\Serverborn.exe""", Firewall.CommandLine(new[] { 7777 }, @"C:\Games\Wow!\Serverborn.exe"));
			Assert.Contains("program=\"" + spaced + "\"", Firewall.CommandLine(new[] { 7777 }, spaced));

			// Characters cmd would still act on inside quotes are refused rather than run.
			Assert.Null(Firewall.CommandLine(new[] { 7777 }, @"C:\Games\100%\Serverborn.exe"));
			Assert.Null(Firewall.CommandLine(new[] { 7777 }, "C:\\Games\\a\"b\\Serverborn.exe"));
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
		public async Task Answers_from_another_matchmaking_version_do_not_throw()
		{
			using var t = new TempPanel();
			AdminClient client = t.Client();

			// A matchmaking from before gamePortsInUse: the ports in use are taken to be the saved ones.
			t.Api.Status = new { version = (string?)null, publicIp = "203.0.113.10", gamePorts = new[] { 7777 }, gamesRunning = 0, requireKeys = true };
			var status = await client.GetStatusAsync();
			Assert.True(status.Ok, status.Message);
			Assert.Equal(new[] { 7777 }, status.Value!.GamePortsInUse);
			Assert.Equal("", status.Value.Version);

			// No port list at all: unreadable, not a crash.
			t.Api.Status = new { version = "1.0.7", publicIp = "203.0.113.10" };
			status = await client.GetStatusAsync();
			Assert.False(status.Ok);
			Assert.Equal(AdminClient.UnreadableMessage, status.Message);

			// Missing strings and lists inside games are filled in; a null game is dropped.
			t.Api.Games = new object?[]
			{
				new { publicId = "g1", port = 7777 },
				null,
				new { publicId = "g2", name = "x", mapMode = (string?)null, players = new object?[] { new { team = 0 }, null } },
			};
			var games = await client.GetGamesAsync();
			Assert.True(games.Ok, games.Message);
			Assert.Equal(2, games.Value!.Count);
			Assert.All(games.Value, g =>
			{
				Assert.NotNull(g.Name);
				Assert.NotNull(g.MapMode);
				Assert.NotNull(g.DraftStyle);
				Assert.NotNull(g.Players);
			});
			PlayerInfo p = Assert.Single(games.Value[1].Players!);
			Assert.Equal("", p.Name);
			Assert.Equal("", p.Hero);

			t.Api.Games = new { notAList = true };
			Assert.Equal(AdminClient.UnreadableMessage, (await client.GetGamesAsync()).Message);

			// No user list: unreadable. A user without a name is dropped.
			t.Api.Users = new { requireKeys = true };
			Assert.Equal(AdminClient.UnreadableMessage, (await client.GetUsersAsync()).Message);
			t.Api.Users = new { requireKeys = true, users = new object?[] { new { name = "Kaz" }, new { canCreateGames = true }, null } };
			var users = await client.GetUsersAsync();
			Assert.True(users.Ok, users.Message);
			Assert.Equal("Kaz", Assert.Single(users.Value!.Users).Name);
		}

		[Fact]
		public async Task A_failing_process_runner_gives_a_message_not_an_exception()
		{
			using var t = new TempPanel();
			t.Runner.Throws = true;
			var control = new MatchmakingControl(t.Paths, t.Runner);

			var (ok, message) = await control.StartAsync();
			Assert.False(ok);
			Assert.StartsWith("Could not start matchmaking: ", message);
			Assert.Contains("Access is denied", message);

			(ok, message) = await control.RestartAsync();
			Assert.False(ok);
			Assert.StartsWith("Could not stop matchmaking: ", message);

			// Through the window's buttons too.
			MainViewModel main = t.Main();
			await main.StartMatchmakingCommand.ExecuteAsync(null);
			Assert.StartsWith("Could not start matchmaking: ", main.Banner);
			await main.RefreshAsync();
			t.Dialogs.ConfirmAnswer = true;
			await main.Ports.RestartCommand.ExecuteAsync(null);
			Assert.StartsWith("Could not stop matchmaking: ", main.Ports.Message);
			await main.Ports.SaveCommand.ExecuteAsync(null);    // the firewall step throws
			Assert.Contains("firewall", main.Ports.Message, StringComparison.OrdinalIgnoreCase);
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
