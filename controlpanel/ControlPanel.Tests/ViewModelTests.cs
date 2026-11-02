using System.Text.Json;
using ControlPanel.Core;
using ControlPanel.Core.ViewModels;

namespace ControlPanel.Tests
{
	public class ViewModelTests
	{
		private static object Game(string id, int port, params (string name, string hero, int team)[] players) => new
		{
			publicId = id,
			name = "Game " + id,
			mapMode = "Versus - Incursion",
			draftStyle = "BanPick5v5",
			randomHero = false,
			port,
			startedUtc = DateTime.UtcNow.AddMinutes(-12).ToString("O"),
			matchStarted = true,
			isRoster = false,
			startedBy = "Sintax",
			players = players.Select(p => new { p.name, p.hero, p.team }).ToArray(),
		};

		[Fact]
		public async Task Refresh_with_matchmaking_down_shows_the_banner_and_recovers()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			t.Api.Down = true;

			await main.RefreshAsync();
			Assert.False(main.MatchmakingUp);
			Assert.Equal("Matchmaking is not running.", main.Banner);
			Assert.True(main.StartMatchmakingCommand.CanExecute(null));

			// Start: launches matchmaking from its folder, and makes Serverborn.exe first (as StartMatchmaking.bat).
			await main.StartMatchmakingCommand.ExecuteAsync(null);
			Assert.Equal(new[] { t.Paths.MatchmakingExe }, t.Runner.Started);
			Assert.True(File.Exists(t.Paths.ServerExe));
			Assert.Contains("Starting matchmaking", main.Banner);

			// Back up: banner gone, header and lists filled.
			t.Api.Down = false;
			t.Api.Games = new[] { Game("g1", 7777, ("Kaz", "Oscar Mike", 0), ("Sintax", "", 1)) };
			await main.RefreshAsync();
			Assert.True(main.MatchmakingUp);
			Assert.Null(main.Banner);
			Assert.Equal("1.0.7", main.Version);
			Assert.Equal("203.0.113.10:5000", main.ServerAddress);
			Assert.Single(main.Games.Games);
			Assert.Equal(2, main.Games.Games[0].Players.Count);
			Assert.Equal("Oscar Mike", main.Games.Games[0].Players[0].Hero);
			Assert.Equal(3, main.Users.Users.Count);
			Assert.Equal(new[] { 7777, 7778 }, main.Ports.Ports);

			// Down again: no exception, banner back, the old lists are not shown as if they were live.
			t.Api.Down = true;
			await main.RefreshAsync();
			Assert.False(main.MatchmakingUp);
			Assert.Equal("Matchmaking is not running.", main.Banner);
			Assert.Empty(main.Games.Games);

			// Already running but not answering: no second copy is started.
			t.Runner.Running.Add("gamecontroller");
			t.Runner.Started.Clear();
			await main.StartMatchmakingCommand.ExecuteAsync(null);
			Assert.Empty(t.Runner.Started);
		}

		[Fact]
		public async Task Generate_key_changes_each_press_and_save_sends_it()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UsersViewModel users = main.Users;

			users.NewName = "Pip";
			users.NewCanCreate = true;
			Assert.Null(users.PreviewKey);
			users.GenerateKeyCommand.Execute(null);
			string first = users.PreviewKey!;
			users.GenerateKeyCommand.Execute(null);
			string second = users.PreviewKey!;
			Assert.True(Keys.ValidKey(first));
			Assert.True(Keys.ValidKey(second));
			Assert.NotEqual(first, second);

			await users.SaveCommand.ExecuteAsync(null);
			FakeApi.Sent post = Assert.Single(t.Api.Writes);
			Assert.Equal("POST", post.Method);
			Assert.Equal("/api/admin/users", post.Path);
			using (var body = JsonDocument.Parse(post.Body!))
			{
				Assert.Equal("Pip", body.RootElement.GetProperty("name").GetString());
				Assert.Equal(second, body.RootElement.GetProperty("key").GetString());
				Assert.True(body.RootElement.GetProperty("canCreateGames").GetBoolean());
			}
			// The form is cleared, and the key is not left in the message.
			Assert.Null(users.PreviewKey);
			Assert.Equal("", users.NewName);
			Assert.Contains("Pip", users.Message);
			Assert.DoesNotContain(second, users.Message);

			// A refusal keeps the form so the owner can fix it, and shows the API's words.
			users.NewName = "Kaz";
			users.GenerateKeyCommand.Execute(null);
			t.Api.Answer = s => s.Method == "POST" ? FakeApi.Message(409, "That name is taken.") : null;
			await users.SaveCommand.ExecuteAsync(null);
			Assert.Equal("That name is taken.", users.Message);
			Assert.NotNull(users.PreviewKey);

			// Bad name or no key yet: nothing is sent.
			int sent = t.Api.Writes.Count();
			users.NewName = "bad name!";
			await users.SaveCommand.ExecuteAsync(null);
			users.NewName = "Pip2";
			users.PreviewKey = null;
			await users.SaveCommand.ExecuteAsync(null);
			Assert.Equal(sent, t.Api.Writes.Count());
		}

		[Fact]
		public async Task Revoke_needs_confirmation()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UserRow kaz = main.Users.Users.Single(u => u.Name == "Kaz");

			t.Dialogs.ConfirmAnswer = false;
			await kaz.RevokeCommand.ExecuteAsync(null);
			Assert.Single(t.Dialogs.Questions);
			Assert.Contains("Kaz", t.Dialogs.Questions[0]);
			Assert.Empty(t.Api.Writes);

			t.Dialogs.ConfirmAnswer = true;
			await kaz.RevokeCommand.ExecuteAsync(null);
			FakeApi.Sent delete = Assert.Single(t.Api.Writes);
			Assert.Equal("DELETE", delete.Method);
			Assert.Equal("/api/admin/users/Kaz", delete.Path);
			Assert.Contains("Kaz", main.Users.Message);

			// The lockout refusal comes back in the API's words.
			t.Api.Answer = s => s.Method == "DELETE" ? FakeApi.Message(409, "At least one user must be able to start games.") : null;
			await main.Users.Users.Single(u => u.Name == "Sintax").RevokeCommand.ExecuteAsync(null);
			Assert.Equal("At least one user must be able to start games.", main.Users.Message);
		}

		[Fact]
		public async Task User_rows_show_the_spec_columns_and_row_actions_call_the_api()
		{
			using var t = new TempPanel();
			File.WriteAllText(t.Paths.ZipRecordFile, "{ \"kaz\": \"2026-10-21T12:00:00Z\" }");
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UsersViewModel users = main.Users;

			UserRow dj = users.Users.Single(u => u.Name == "dj");
			Assert.Equal("confirm name", dj.NameNote);
			Assert.Equal("never", dj.LastSeenText);
			UserRow kaz = users.Users.Single(u => u.Name == "Kaz");
			Assert.Equal("", kaz.NameNote);
			Assert.False(kaz.CanCreateGames);
			Assert.NotEqual("", kaz.KeyMadeText);
			Assert.NotEqual("", kaz.ZipMadeText);
			Assert.Equal("", users.Users.Single(u => u.Name == "Sintax").ZipMadeText);
			Assert.True(users.RequireKeys);

			await kaz.ToggleCreateCommand.ExecuteAsync(null);
			FakeApi.Sent put = t.Api.Writes.Last();
			Assert.Equal(("PUT", "/api/admin/users/Kaz"), (put.Method, put.Path));
			Assert.Contains("\"canCreateGames\":true", put.Body);

			t.Dialogs.TextAnswer = "Kazzy";
			await kaz.RenameCommand.ExecuteAsync(null);
			put = t.Api.Writes.Last();
			Assert.Contains("\"newName\":\"Kazzy\"", put.Body);
			Assert.False(ZipRecords.Load(t.Paths.ZipRecordFile).ContainsKey("Kaz"));   // the old zip carries the old name

			t.Dialogs.ConfirmAnswer = true;
			await kaz.NewKeyCommand.ExecuteAsync(null);
			put = t.Api.Writes.Last();
			Assert.Equal("/api/admin/users/Kaz/key", put.Path);
			string key = JsonDocument.Parse(put.Body!).RootElement.GetProperty("key").GetString()!;
			Assert.True(Keys.ValidKey(key));
			Assert.DoesNotContain(key, users.Message);

			// Turning keys off asks first.
			t.Dialogs.ConfirmAnswer = false;
			int before = t.Api.Writes.Count();
			await users.ToggleRequireKeysCommand.ExecuteAsync(null);
			Assert.Equal(before, t.Api.Writes.Count());
			Assert.True(users.RequireKeys);
			t.Dialogs.ConfirmAnswer = true;
			await users.ToggleRequireKeysCommand.ExecuteAsync(null);
			Assert.Contains("\"on\":false", t.Api.Writes.Last().Body);
			Assert.False(users.RequireKeys);
		}

		[Fact]
		public async Task Make_zip_writes_the_friend_zip_and_marks_the_row()
		{
			using var t = new TempPanel();
			using (var zip = System.IO.Compression.ZipFile.Open(t.Paths.PlayerInstallerZip, System.IO.Compression.ZipArchiveMode.Create))
			{
				zip.CreateEntry("Install.bat");
				zip.CreateEntry("files/reborn.dll");
			}
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UserRow kaz = main.Users.Users.Single(u => u.Name == "Kaz");
			Assert.Equal("", kaz.ZipMadeText);

			await kaz.MakeZipCommand.ExecuteAsync(null);
			Assert.Equal("/api/admin/users/Kaz/key", t.Api.Requests.Last(r => r.Method == "GET" && r.Path.EndsWith("/key")).Path);
			string made = Path.Combine(t.Paths.ZipOutputDir, "Reborn-Mod-Installer_Kaz.zip");
			Assert.True(File.Exists(made));
			Assert.NotEqual("", kaz.ZipMadeText);
			Assert.Contains("Reborn-Mod-Installer_Kaz.zip", main.Users.Message);
			Assert.DoesNotContain("ABCDEFGHJKMNPQRSTVWXYZ23", main.Users.Message);
			main.Users.OpenZipFolderCommand.Execute(null);
			Assert.Equal(new[] { t.Paths.ZipOutputDir }, t.Runner.Opened);

			// Without the installer zip: a plain message.
			File.Delete(t.Paths.PlayerInstallerZip);
			await kaz.MakeZipCommand.ExecuteAsync(null);
			Assert.Contains("player installer zip is missing", main.Users.Message);
		}

		[Fact]
		public async Task With_no_user_list_the_page_says_anyone_can_join_and_the_first_add_warns()
		{
			using var t = new TempPanel();
			t.Api.Users = new { requireKeys = false, userListExists = false, users = Array.Empty<object>() };
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UsersViewModel users = main.Users;

			Assert.False(users.HasUserList);
			Assert.False(users.RequireKeys);                      // the switch does not claim keys are required
			Assert.Contains("anyone can join", users.NoListNote);

			// The switch does nothing on its own while there is no list.
			t.Dialogs.ConfirmAnswer = true;
			await users.ToggleRequireKeysCommand.ExecuteAsync(null);
			Assert.Empty(t.Api.Writes);
			Assert.Contains("anyone can join", users.Message);

			// The first Add asks first: from now on only listed players can join.
			users.NewName = "Sintax";
			users.NewCanCreate = true;
			users.GenerateKeyCommand.Execute(null);
			t.Dialogs.Questions.Clear();
			t.Dialogs.ConfirmAnswer = false;
			await users.SaveCommand.ExecuteAsync(null);
			string question = Assert.Single(t.Dialogs.Questions);
			Assert.Contains("only players on the list can join", question);
			Assert.Empty(t.Api.Writes);
			Assert.NotNull(users.PreviewKey);                     // the form is kept

			// Without "Can create games", the question also says nobody could start a game.
			users.NewCanCreate = false;
			await users.SaveCommand.ExecuteAsync(null);
			Assert.Contains("Can create games", t.Dialogs.Questions.Last());

			users.NewCanCreate = true;
			t.Dialogs.ConfirmAnswer = true;
			await users.SaveCommand.ExecuteAsync(null);
			Assert.Equal("POST", Assert.Single(t.Api.Writes).Method);

			// Once the list exists, adding asks nothing more.
			t.Api.Users = new
			{
				requireKeys = true,
				userListExists = true,
				users = new object[] { new { name = "Sintax", canCreateGames = true, confirmName = false, added = (string?)null, lastSeen = (string?)null } },
			};
			await main.RefreshAsync();
			Assert.True(users.HasUserList);
			Assert.True(users.RequireKeys);
			Assert.Equal("", users.NoListNote);
			int asked = t.Dialogs.Questions.Count;
			users.NewName = "Kaz";
			users.GenerateKeyCommand.Execute(null);
			await users.SaveCommand.ExecuteAsync(null);
			Assert.Equal(asked, t.Dialogs.Questions.Count);
			Assert.Equal(2, t.Api.Writes.Count());
		}

		[Fact]
		public async Task Users_page_lists_names_seen_recently_that_are_not_on_the_list()
		{
			using var t = new TempPanel();
			var clock = new ManualClock();   // 2026-10-22 12:00 UTC
			MainViewModel main = t.Main(clock);
			await main.RefreshAsync();
			UsersViewModel users = main.Users;

			// An older matchmaking does not send the list: nothing shown, nothing thrown.
			Assert.Empty(users.SeenNotListed);
			Assert.Equal(UsersViewModel.NobodySeenText, users.SeenNotListedEmptyText);

			t.Api.Users = new
			{
				requireKeys = true,
				userListExists = true,
				users = new object[] { new { name = "kaz", canCreateGames = true, confirmName = true, added = (string?)null, lastSeen = (string?)null } },
				seenNotListed = new object?[]
				{
					new { name = "KazTheGreat", lastSeen = "2026-10-22T11:55:00Z" },
					null,
					new { name = (string?)null, lastSeen = "2026-10-22T11:00:00Z" },
					new { name = "Wanderer", lastSeen = "2026-10-20T08:00:00Z" },
				},
			};
			await main.RefreshAsync();

			// In the order matchmaking sends (newest first); broken entries dropped.
			Assert.Equal(new[] { "KazTheGreat", "Wanderer" }, users.SeenNotListed.Select(s => s.Name));
			Assert.Equal("5 min ago", users.SeenNotListed[0].LastSeenText);
			Assert.Equal("", users.SeenNotListedEmptyText);

			// The list leaves the page once they're all on the list.
			t.Api.Users = new { requireKeys = true, userListExists = true, users = Array.Empty<object>(), seenNotListed = Array.Empty<object>() };
			await main.RefreshAsync();
			Assert.Empty(users.SeenNotListed);
		}

		[Fact]
		public async Task Turning_keys_on_asks_first()
		{
			using var t = new TempPanel();
			t.Api.Users = new { requireKeys = false, users = Array.Empty<object>() };   // an older matchmaking: no userListExists
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UsersViewModel users = main.Users;
			Assert.True(users.HasUserList);
			Assert.False(users.RequireKeys);

			t.Dialogs.ConfirmAnswer = false;
			await users.ToggleRequireKeysCommand.ExecuteAsync(null);
			Assert.Contains("Players without a key will be dropped from games.", Assert.Single(t.Dialogs.Questions));
			Assert.Empty(t.Api.Writes);
			Assert.False(users.RequireKeys);

			t.Dialogs.ConfirmAnswer = true;
			await users.ToggleRequireKeysCommand.ExecuteAsync(null);
			Assert.Contains("\"on\":true", Assert.Single(t.Api.Writes).Body);
			Assert.True(users.RequireKeys);
		}

		[Fact]
		public async Task Make_zip_waits_until_a_confirm_name_row_is_confirmed()
		{
			using var t = new TempPanel();
			using (var zip = System.IO.Compression.ZipFile.Open(t.Paths.PlayerInstallerZip, System.IO.Compression.ZipArchiveMode.Create))
			{
				zip.CreateEntry("Install.bat");
				zip.CreateEntry("files/reborn.dll");
			}
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UserRow dj = main.Users.Users.Single(u => u.Name == "dj");      // a key brought over from the old key files
			UserRow kaz = main.Users.Users.Single(u => u.Name == "Kaz");

			// "dj" is only a label: a zip would make it their in-game name. So no zip until it is confirmed.
			Assert.False(dj.MakeZipCommand.CanExecute(null));
			Assert.Equal("Confirm their name first", dj.MakeZipNote);
			Assert.True(kaz.MakeZipCommand.CanExecute(null));
			Assert.Equal("", kaz.MakeZipNote);

			// Even if it is run anyway: no key is fetched and no zip is made.
			await dj.MakeZipCommand.ExecuteAsync(null);
			Assert.DoesNotContain(t.Api.Requests, r => r.Path.EndsWith("/key"));
			Assert.False(File.Exists(Path.Combine(t.Paths.ZipOutputDir, "Reborn-Mod-Installer_dj.zip")));
			Assert.Contains("Confirm their name first", main.Users.Message);

			// Once the name is confirmed, Make zip works.
			t.Api.Users = new
			{
				requireKeys = true,
				users = new object[] { new { name = "dj", canCreateGames = true, confirmName = false, added = (string?)null, lastSeen = (string?)null } },
			};
			await main.RefreshAsync();
			Assert.True(dj.MakeZipCommand.CanExecute(null));
			Assert.Equal("", dj.MakeZipNote);
		}

		[Fact]
		public async Task Rename_and_confirm_name_say_what_happens_before_saving()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			UserRow kaz = main.Users.Users.Single(u => u.Name == "Kaz");
			UserRow dj = main.Users.Users.Single(u => u.Name == "dj");

			// Rename: the question says it needs a new zip and drops them from a game now. Cancel = nothing.
			t.Dialogs.TextAnswer = null;
			await kaz.RenameCommand.ExecuteAsync(null);
			string rename = Assert.Single(t.Dialogs.Questions);
			Assert.Contains("Kaz", rename);
			Assert.Contains("new zip", rename);
			Assert.Contains("in a game now, they'll be dropped", rename);
			Assert.Empty(t.Api.Writes);

			// Confirm name: type it exactly as it appears in the game. Being dropped is only for a name that
			// doesn't match; the right name changes nothing for them.
			t.Dialogs.Questions.Clear();
			await dj.RenameCommand.ExecuteAsync(null);
			string confirm = Assert.Single(t.Dialogs.Questions);
			Assert.Contains("exactly as it appears in the game", confirm);
			Assert.Contains("If it doesn't match", confirm);
			Assert.DoesNotContain("new zip", confirm);
			Assert.Empty(t.Api.Writes);

			// Confirming the name the label already has: saved, and nobody is told they were dropped.
			t.Dialogs.TextAnswer = "dj";
			await dj.RenameCommand.ExecuteAsync(null);
			Assert.Contains("\"newName\":\"dj\"", t.Api.Writes.Last().Body);
			Assert.DoesNotContain("dropped", main.Users.Message);
			Assert.DoesNotContain("new zip", main.Users.Message);

			// A real rename: the message says to send the new zip.
			t.Dialogs.TextAnswer = "Kazzy";
			await kaz.RenameCommand.ExecuteAsync(null);
			Assert.Contains("new zip", main.Users.Message);
		}

		[Fact]
		public async Task Removing_a_busy_port_warns()
		{
			using var t = new TempPanel();
			t.Api.Games = new[] { Game("g1", 7778, ("Kaz", "Oscar Mike", 0)) };
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			PortsViewModel ports = main.Ports;
			Assert.Equal(new[] { 7777, 7778 }, ports.Ports);
			Assert.Equal("Also open these UDP ports in your hosting company's firewall.", ports.Reminder);
			Assert.Null(ports.RestartNote);

			ports.RemovePortCommand.Execute(7777);
			Assert.Null(ports.Warning);
			ports.RemovePortCommand.Execute(7778);                       // the last one stays
			Assert.Equal(new[] { 7778 }, ports.Ports);
			ports.NewPort = "7777";
			ports.AddPortCommand.Execute(null);
			ports.RemovePortCommand.Execute(7778);                       // a game is running on it
			Assert.Equal(new[] { 7777 }, ports.Ports);
			Assert.Contains("7778", ports.Warning);
			Assert.Contains("keeps running", ports.Warning);

			await ports.SaveCommand.ExecuteAsync(null);
			FakeApi.Sent put = t.Api.Writes.Single();
			Assert.Equal("/api/admin/ports", put.Path);
			Assert.Equal("{\"ports\":[7777]}", put.Body);
			// One run as administrator does both the delete and the add (one Windows question, and no
			// chance of the rule being deleted but not added back). The busy port stays open until the
			// restart, so the running game is not cut off.
			string run = Assert.Single(t.Runner.Elevated);
			Assert.Equal("cmd.exe " + Firewall.CommandLine(new[] { 7777, 7778 }, t.Paths.ServerExe), run);
			Assert.StartsWith("cmd.exe /d /s /v:off /c \"netsh advfirewall firewall delete rule", run);
			Assert.Contains(" & netsh advfirewall firewall add rule", run);
			Assert.Contains("localport=7777,7778 ", run);
			Assert.Contains(t.Paths.ServerExe, run);
			Assert.StartsWith("Saved. Windows Firewall lets players in", ports.Message);

			// Matchmaking now reports the saved list; the panel says a restart is needed.
			t.Api.Status = new { version = "1.0.7", publicIp = "203.0.113.10", gamePorts = new[] { 7777 }, gamePortsInUse = new[] { 7777, 7778 },
				gamesRunning = 1, requireKeys = true, matchmakingStartedUtc = "2026-10-22T11:22:35.06Z" };
			await main.RefreshAsync();
			Assert.Equal("Restart matchmaking to use the new ports.", ports.RestartNote);

			// Restart warns that players are in a game, and does nothing without a yes.
			t.Dialogs.ConfirmAnswer = false;
			await ports.RestartCommand.ExecuteAsync(null);
			Assert.Contains("1 player", t.Dialogs.Questions.Last());
			Assert.Empty(t.Runner.Stopped);
			t.Dialogs.ConfirmAnswer = true;
			t.Runner.Elevated.Clear();
			await ports.RestartCommand.ExecuteAsync(null);
			Assert.Equal(new[] { "gamecontroller", "Serverborn" }, t.Runner.Stopped);
			Assert.Contains(t.Paths.MatchmakingExe, t.Runner.Started);
			Assert.Contains("localport=7777 ", Assert.Single(t.Runner.Elevated));   // the old port is closed now
		}

		[Fact]
		public async Task Save_and_restart_cannot_run_at_the_same_time()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			PortsViewModel ports = main.Ports;
			Assert.True(ports.SaveCommand.CanExecute(null));
			Assert.True(ports.RestartCommand.CanExecute(null));

			// Save waits on the Windows question...
			t.Runner.ElevatedGate = new TaskCompletionSource<int>();
			Task saving = ports.SaveCommand.ExecuteAsync(null);
			Assert.False(saving.IsCompleted);
			Assert.True(ports.Busy);
			Assert.False(ports.SaveCommand.CanExecute(null));
			Assert.False(ports.RestartCommand.CanExecute(null));

			// ...so a restart pressed meanwhile does nothing, not even ask.
			t.Dialogs.ConfirmAnswer = true;
			await ports.RestartCommand.ExecuteAsync(null);
			Assert.Empty(t.Dialogs.Questions);
			Assert.Empty(t.Runner.Stopped);

			t.Runner.ElevatedGate.SetResult(0);
			await saving;
			Assert.False(ports.Busy);
			Assert.True(ports.SaveCommand.CanExecute(null));
			Assert.True(ports.RestartCommand.CanExecute(null));

			// And the other way round: a save pressed during a restart does nothing. (The restart closes
			// a dropped port in the firewall while matchmaking is stopped, and waits on that question.)
			t.Api.Status = new { version = "1.0.7", publicIp = "203.0.113.10", gamePorts = new[] { 7777 }, gamePortsInUse = new[] { 7777, 7778 },
				gamesRunning = 0, requireKeys = true, matchmakingStartedUtc = "2026-10-22T11:22:35.06Z" };
			await main.RefreshAsync();
			t.Runner.ElevatedGate = new TaskCompletionSource<int>();
			int writes = t.Api.Writes.Count();
			Task restarting = ports.RestartCommand.ExecuteAsync(null);
			Assert.False(restarting.IsCompleted);
			Assert.True(ports.Busy);
			Assert.False(ports.SaveCommand.CanExecute(null));
			await ports.SaveCommand.ExecuteAsync(null);
			Assert.Equal(writes, t.Api.Writes.Count());

			t.Runner.ElevatedGate.SetResult(0);
			await restarting;
			Assert.False(ports.Busy);
			Assert.Contains(t.Paths.MatchmakingExe, t.Runner.Started);
		}

		[Fact]
		public async Task Firewall_messages_say_what_really_happened()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			PortsViewModel ports = main.Ports;

			t.Runner.ElevatedExitCode = -1;            // the owner said no to Windows
			await ports.SaveCommand.ExecuteAsync(null);
			Assert.Contains("Windows Firewall was not changed.", ports.Message);

			t.Runner.ElevatedExitCode = 1;             // netsh failed
			await ports.SaveCommand.ExecuteAsync(null);
			Assert.Contains("Windows Firewall update failed", ports.Message);
			Assert.Contains("Save again", ports.Message);
			Assert.DoesNotContain("was not changed", ports.Message);

			t.Runner.ElevatedExitCode = 0;
			await ports.SaveCommand.ExecuteAsync(null);
			Assert.StartsWith("Saved. Windows Firewall lets players in on UDP 7777, 7778.", ports.Message);
			Assert.Equal(3, t.Runner.Elevated.Count);  // one Windows question per save
		}

		[Fact]
		public async Task Refresh_with_an_older_matchmaking_answer_does_not_throw()
		{
			using var t = new TempPanel();
			t.Api.Status = new { version = "1.0.6", publicIp = "203.0.113.10", gamePorts = new[] { 7777, 7778 }, gamesRunning = 1, requireKeys = true };
			t.Api.Games = new object[] { new { publicId = "g1", port = 7777, mapMode = (string?)null, players = (object?)null } };
			t.Api.Users = new { requireKeys = true, users = (object?)null };
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			Assert.True(main.MatchmakingUp);
			Assert.Equal(new[] { 7777, 7778 }, main.Ports.Ports);
			Assert.Null(main.Ports.RestartNote);
			GameRow g = Assert.Single(main.Games.Games);
			Assert.Empty(g.Players);
			Assert.Equal(AdminClient.UnreadableMessage, main.Users.Message);
		}

		[Fact]
		public async Task Ports_add_checks_and_memory_plan()
		{
			using var t = new TempPanel();
			t.Memory.Reading = new MemoryReading(6.0, 2.1);
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			PortsViewModel ports = main.Ports;
			Assert.Equal("Each game uses about 1.3 GB. 2 games need about 2.6 GB; this server has 6.0 GB (2.1 GB free).", ports.MemoryPlan!.Summary);

			foreach (string bad in new[] { "", "abc", "80", "70000", "7777" })
			{
				ports.NewPort = bad;
				ports.AddPortCommand.Execute(null);
				Assert.Equal(2, ports.Ports.Count);
				Assert.False(string.IsNullOrEmpty(ports.Message));
			}
			ports.NewPort = "7779";
			ports.AddPortCommand.Execute(null);
			ports.NewPort = " 7780 ";
			ports.AddPortCommand.Execute(null);
			Assert.Equal(new[] { 7777, 7778, 7779, 7780 }, ports.Ports);
			Assert.True(ports.HasChanges);
			Assert.True(ports.MemoryPlan!.TooMany);

			// A refresh does not throw away changes not saved yet.
			await main.RefreshAsync();
			Assert.Equal(4, ports.Ports.Count);

			// Firewall refused (the owner said no to Windows): saved, but says so.
			t.Runner.ElevatedExitCode = 1;
			await ports.SaveCommand.ExecuteAsync(null);
			Assert.Contains("firewall", ports.Message, StringComparison.OrdinalIgnoreCase);
			Assert.False(ports.HasChanges);
		}

		[Fact]
		public async Task Copy_address_puts_ip_and_port_on_the_clipboard()
		{
			using var t = new TempPanel();
			MainViewModel main = t.Main();
			Assert.False(main.CopyAddressCommand.CanExecute(null));
			await main.RefreshAsync();
			Assert.True(main.CopyAddressCommand.CanExecute(null));
			main.CopyAddressCommand.Execute(null);
			Assert.Equal("203.0.113.10:5000", t.Clipboard.Text);
		}

		[Fact]
		public async Task Clipboard_busy_notice_survives_a_refresh_and_clears_later()
		{
			using var t = new TempPanel();
			var clock = new ManualClock();
			MainViewModel main = t.Main(clock);
			await main.RefreshAsync();

			t.Clipboard.Busy = true;
			main.CopyAddressCommand.Execute(null);
			Assert.Equal(MainViewModel.CopyBusyText, main.CopyNotice);
			Assert.Null(main.Banner);   // not in the banner, which every refresh rewrites

			// The next refresh (5 s later) keeps it.
			clock.Now += TimeSpan.FromSeconds(5);
			await main.RefreshAsync();
			Assert.Equal(MainViewModel.CopyBusyText, main.CopyNotice);

			// About 15 s on, a refresh clears it.
			clock.Now += TimeSpan.FromSeconds(11);
			await main.RefreshAsync();
			Assert.Null(main.CopyNotice);

			// A Copy that works clears it at once.
			main.CopyAddressCommand.Execute(null);
			Assert.NotNull(main.CopyNotice);
			t.Clipboard.Busy = false;
			main.CopyAddressCommand.Execute(null);
			Assert.Null(main.CopyNotice);
			Assert.Equal("203.0.113.10:5000", t.Clipboard.Text);
		}

		[Fact]
		public async Task Games_show_mode_port_age_and_players()
		{
			using var t = new TempPanel();
			t.Api.Games = new[] { Game("g1", 7777, ("Kaz", "Oscar Mike", 0), ("Sintax", "", 1)) };
			MainViewModel main = t.Main();
			await main.RefreshAsync();
			GameRow g = Assert.Single(main.Games.Games);
			Assert.Equal("Game g1", g.Name);
			Assert.Equal("Versus - Incursion", g.Map);
			Assert.Equal("Versus, ban and pick", g.Mode);
			Assert.Equal(7777, g.Port);
			Assert.Equal("12 min", g.RunningFor);
			Assert.Equal("Sintax", g.StartedBy);
			Assert.Equal("choosing a hero", g.Players[1].Hero);
			Assert.Equal("Team 2", g.Players[1].Team);
			Assert.Equal("Team 1", g.Players[0].Team);
			Assert.Equal("", main.Games.EmptyText);

			// Story and Operations: everyone is on one team, so no team is shown.
			foreach (string mapMode in new[] { "Story - The Algorithm", "Operations - Attikus" })
			{
				t.Api.Games = new[] { new
				{
					publicId = "s1", name = "Co-op", mapMode, draftStyle = "", randomHero = false, port = 7778,
					startedUtc = DateTime.UtcNow.ToString("O"), matchStarted = true, isRoster = false, startedBy = "Sintax",
					players = new[] { new { name = "Kaz", hero = "Rath", team = 0 } },
				} };
				await main.RefreshAsync();
				Assert.Equal("", Assert.Single(Assert.Single(main.Games.Games).Players).Team);
			}

			t.Api.Games = Array.Empty<object>();
			await main.RefreshAsync();
			Assert.Empty(main.Games.Games);
			Assert.Equal("No games are running.", main.Games.EmptyText);
		}

		[Fact]
		public void Logs_show_the_last_200_lines_and_open_the_game_logs()
		{
			using var t = new TempPanel();
			var logs = new LogsViewModel(t.Paths, t.Runner);
			logs.RefreshCommand.Execute(null);
			Assert.Equal("", logs.Text);
			Assert.Contains("no matchmaking log", logs.Message);

			// Matchmaking keeps the log open for writing; the panel can still read it.
			using (var writer = new StreamWriter(new FileStream(t.Paths.MatchmakingLog, FileMode.Create, FileAccess.Write, FileShare.ReadWrite)))
			{
				for (int i = 1; i <= 500; i++) writer.WriteLine("line " + i);
				writer.Flush();
				logs.RefreshCommand.Execute(null);
			}
			string[] lines = logs.Text.Split(Environment.NewLine);
			Assert.Equal(200, lines.Length);
			Assert.Equal("line 301", lines[0]);
			Assert.Equal("line 500", lines[^1]);

			logs.OpenGameLogsCommand.Execute(null);
			Assert.Equal(new[] { t.Paths.GameLogsDir }, t.Runner.Opened);
		}
	}
}
