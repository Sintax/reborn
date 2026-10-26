using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using ControlPanel.Core;
using ControlPanel.Core.ViewModels;
using Reborn.Update;

namespace ControlPanel.Tests
{
	// A temp panel plus a pretend GitHub: the fake runner answers `gh release download` by writing the
	// prepared update.json, its signature and the zip into the --dir it was given. Signed with a
	// throw-away key made for each test.
	public sealed class UpdateRig : IDisposable
	{
		public TempPanel T { get; } = new();
		public ECDsa Key { get; } = ECDsa.Create(ECCurve.NamedCurves.nistP256);
		public string TempDir { get; }   // stands in for %TEMP%
		public ManualClock Clock { get; } = new();
		public UpdateService Service { get; }
		public UpdatesViewModel Vm { get; }
		public int Players { get; set; }

		public byte[]? ManifestBytes { get; set; }
		public string? Sig { get; set; }
		public byte[]? Zip { get; set; }
		public int GhExit { get; set; }
		public string GhOutput { get; set; } = "";

		public UpdateRig()
		{
			TempDir = Path.Combine(T.Root, "temp");
			Directory.CreateDirectory(TempDir);
			T.App.ExePath = Path.Combine(T.Win64, "ControlPanel", "ControlPanel.exe");
			Service = new UpdateService(T.Paths, T.Runner, PublicKeyOf(Key), TempDir);
			Vm = new UpdatesViewModel(Service, T.Dialogs, T.App, () => Players, Clock);
			T.Runner.OnRun = Gh;
		}

		public string StagingDir => Path.Combine(T.Win64, "RebornVersions", "staging");

		public void Deployed(string text) => File.WriteAllText(Path.Combine(T.Win64, "reborn-deployed.txt"), text);

		// A release on the pretend GitHub, signed with `signer` (the right key unless given).
		public void Publish(string version, string notes = "Fixes the lobby.", byte[]? zip = null, ECDsa? signer = null,
			string? sha256 = null, long? size = null)
		{
			Zip = zip ?? MakeZip();
			string json = JsonSerializer.Serialize(new
			{
				version,
				build = Versions.Parse(version),
				notes,
				commit = "0123456789abcdef0123456789abcdef01234567",
				zip = $"Reborn-Server-Update-{version}.zip",
				sha256 = sha256 ?? Convert.ToHexString(SHA256.HashData(Zip)).ToLowerInvariant(),
				size = size ?? Zip.Length,
			}) + "\n";
			ManifestBytes = Encoding.UTF8.GetBytes(json);
			Sig = Convert.ToBase64String((signer ?? Key).SignData(ManifestBytes, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
		}

		private (int, string) Gh(string file, string args)
		{
			if (GhExit != 0) return (GhExit, GhOutput);
			string dir = Regex.Match(args, "--dir \"([^\"]+)\"").Groups[1].Value;
			if (args.Contains("--pattern update.json --pattern update.json.sig"))
			{
				if (ManifestBytes != null) File.WriteAllBytes(Path.Combine(dir, "update.json"), ManifestBytes);
				if (Sig != null) File.WriteAllText(Path.Combine(dir, "update.json.sig"), Sig);
			}
			else if (Zip != null)
			{
				string name = Regex.Match(args, "--pattern \"([^\"]+)\"").Groups[1].Value;
				File.WriteAllBytes(Path.Combine(dir, name), Zip);
			}
			return (0, "");
		}

		public static string PublicKeyOf(ECDsa key)
		{
			ECParameters p = key.ExportParameters(false);
			return Convert.ToBase64String(p.Q.X!.Concat(p.Q.Y!).ToArray());
		}

		// A server update zip: everything under Win64/, with the updater in Win64/ControlPanel/.
		public static byte[] MakeZip(string updater = "the new updater")
		{
			using var ms = new MemoryStream();
			using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, leaveOpen: true))
			{
				void Add(string name, string text)
				{
					using var w = new StreamWriter(zip.CreateEntry(name).Open());
					w.Write(text);
				}
				Add("Win64/ControlPanel/RebornUpdater.exe", updater);
				Add("Win64/ControlPanel/ControlPanel.exe", "the new panel");
				Add("Win64/reborn.dll", "the new mod");
				Add("Win64/matchmaking/gamecontroller.exe", "the new matchmaking");
			}
			return ms.ToArray();
		}

		public void Dispose()
		{
			Key.Dispose();
			T.Dispose();
		}
	}

	public class UpdateTests
	{
		private const string GhCheck = "gh release download --repo Sintax/reborn-builds --pattern update.json --pattern update.json.sig --dir \"";

		[Fact]
		public async Task check_finds_a_newer_signed_update()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3", "Story missions save again.");

			await r.Vm.CheckCommand.ExecuteAsync(null);

			Assert.Equal("1.0.2", r.Vm.CurrentVersion);
			Assert.Equal("1.0.3", r.Vm.Available);
			Assert.Equal("Story missions save again.", r.Vm.Notes);
			Assert.Equal(UpdateService.ReadyText, r.Vm.Status);   // the heading already says "Update available: 1.0.3"
			Assert.True(r.Vm.UpdateNowCommand.CanExecute(null));
			string ran = Assert.Single(r.T.Runner.Ran);
			Assert.StartsWith(GhCheck, ran);
			Assert.EndsWith("\" --clobber", ran);
			// The download folder for the check is gone again.
			Assert.Empty(Directory.GetFileSystemEntries(r.TempDir));

			// A server deployed by the old tools (a commit, not a number) counts as older than any release.
			r.Deployed("2026-10-22 bab2c4e\n");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal("2026-10-22 bab2c4e", r.Vm.CurrentVersion);
			Assert.Equal("1.0.3", r.Vm.Available);

			// The header says so too.
			using var main = new UpdateRig();
			main.Deployed("1.0.2\n");
			main.Publish("1.0.3");
			MainViewModel m = main.T.Main(main.Clock, main.Service);
			Assert.Equal("", m.UpdateLine);
			await m.RefreshAsync();
			await m.Updates.CheckTask!;
			Assert.Equal("Update available: 1.0.3", m.UpdateLine);
		}

		[Fact]
		public async Task check_rejects_a_bad_signature()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");

			// Signed by some other key.
			using var other = ECDsa.Create(ECCurve.NamedCurves.nistP256);
			r.Publish("1.0.3", signer: other);
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
			Assert.Equal(UpdateService.BadSignatureText, r.Vm.Status);
			Assert.False(r.Vm.UpdateNowCommand.CanExecute(null));

			// The right key, but the text was changed after signing.
			r.Publish("1.0.3");
			r.ManifestBytes = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(r.ManifestBytes!).Replace("Fixes the lobby.", "Fixes the lobby!"));
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
			Assert.Equal(UpdateService.BadSignatureText, r.Vm.Status);

			// No signature file at all.
			r.Publish("1.0.3");
			r.Sig = null;
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
			Assert.Equal(UpdateService.NoSignedUpdateText, r.Vm.Status);

			// A good update found earlier is dropped when a later check fails.
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal("1.0.3", r.Vm.Available);
			r.Publish("1.0.4", signer: other);
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
		}

		[Fact]
		public async Task check_without_github_says_so()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");

			// gh is not installed (cannot start), not signed in, or offline: one plain sentence.
			foreach (var (exit, output) in new[]
			{
				(-1, ""),
				(4, "To get started with GitHub CLI, please run:  gh auth login"),
				(1, "error connecting to api.github.com"),
			})
			{
				r.GhExit = exit;
				r.GhOutput = output;
				await r.Vm.CheckCommand.ExecuteAsync(null);
				Assert.Equal("This server can't reach the builds on GitHub right now. Nothing was changed.", r.Vm.Status);
				Assert.Equal(UpdateService.NoGitHubText, r.Vm.Status);
				Assert.Null(r.Vm.Available);
				Assert.False(r.Vm.Busy);
			}

			// The newest release has no update.json (an old-style release).
			r.GhExit = 1;
			r.GhOutput = "no assets match the file pattern";
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal(UpdateService.NoSignedUpdateText, r.Vm.Status);

			// A runner that breaks its promise and throws is still a plain message.
			r.T.Runner.Throws = true;
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.StartsWith("The update check hit a problem", r.Vm.Status);
			Assert.False(r.Vm.Busy);
		}

		[Fact]
		public async Task same_or_older_build_is_up_to_date()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.3\n");

			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
			Assert.Equal("This server is up to date.", r.Vm.Status);
			Assert.False(r.Vm.UpdateNowCommand.CanExecute(null));

			// An older release, even validly signed, is never offered (no going back by accident).
			r.Publish("1.0.2");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Null(r.Vm.Available);
			Assert.Equal("This server is up to date.", r.Vm.Status);
			Assert.Equal("", r.Vm.Notes);
		}

		[Fact]
		public async Task download_rejects_a_wrong_hash_and_cleans_up()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");

			// The signed description says one hash; the file on GitHub is another.
			r.Publish("1.0.3", sha256: new string('a', 64));
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal("1.0.3", r.Vm.Available);
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);

			Assert.Equal(UpdateService.MismatchText, r.Vm.Status);
			Assert.False(Directory.Exists(r.StagingDir));
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.Equal(0, r.T.App.Closed);
			Assert.False(r.Vm.Busy);
			// That release isn't offered again until a later check finds it.
			Assert.Null(r.Vm.Available);
			Assert.Equal("", r.Vm.Notes);
			Assert.False(r.Vm.UpdateNowCommand.CanExecute(null));
			Assert.Contains("gh release download 1.0.3 --repo Sintax/reborn-builds --pattern \"Reborn-Server-Update-1.0.3.zip\" --dir \""
				+ r.StagingDir + "\" --clobber", r.T.Runner.Ran);

			// The right hash but the wrong size is refused too.
			r.Publish("1.0.3", size: 12345);
			await r.Vm.CheckCommand.ExecuteAsync(null);
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(UpdateService.MismatchText, r.Vm.Status);
			Assert.False(Directory.Exists(r.StagingDir));

			// A good zip without the server files in it.
			using (var ms = new MemoryStream())
			{
				using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, leaveOpen: true)) zip.CreateEntry("readme.txt");
				r.Publish("1.0.3", zip: ms.ToArray());
			}
			await r.Vm.CheckCommand.ExecuteAsync(null);
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(UpdateService.NoServerFilesText, r.Vm.Status);
			Assert.False(Directory.Exists(r.StagingDir));

			// GitHub gone while downloading.
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			r.GhExit = 1;
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(UpdateService.NoGitHubText, r.Vm.Status);
			Assert.False(Directory.Exists(r.StagingDir));
			Assert.Empty(r.T.Runner.StartedWith);
		}

		[Fact]
		public async Task update_now_warns_when_players_are_in_games()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			r.Players = 3;
			int gh = r.T.Runner.Ran.Count;

			r.T.Dialogs.ConfirmAnswer = false;
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(new[] { "3 players are in games. Updating stops those games. Update now?" }, r.T.Dialogs.Questions);
			Assert.Equal(gh, r.T.Runner.Ran.Count);   // nothing downloaded
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.Equal(0, r.T.App.Closed);
			Assert.Equal("1.0.3", r.Vm.Available);

			r.T.Dialogs.ConfirmAnswer = true;
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(2, r.T.Dialogs.Questions.Count);
			Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal(1, r.T.App.Closed);

			// With one player the words fit.
			using var one = new UpdateRig();
			one.Deployed("1.0.2\n");
			one.Publish("1.0.3");
			await one.Vm.CheckCommand.ExecuteAsync(null);
			one.Players = 1;
			await one.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(new[] { "1 player is in a game. Updating stops that game. Update now?" }, one.T.Dialogs.Questions);
		}

		[Fact]
		public async Task update_now_starts_the_updater_from_a_temp_copy()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);

			await r.Vm.UpdateNowCommand.ExecuteAsync(null);

			// Nobody is in a game: no question.
			Assert.Empty(r.T.Dialogs.Questions);

			// The zip is checked and unpacked into RebornVersions\staging\1.0.3\, and the zip itself is gone.
			string staged = Path.Combine(r.StagingDir, "1.0.3", "Win64");
			Assert.Equal("the new mod", File.ReadAllText(Path.Combine(staged, "reborn.dll")));
			Assert.False(File.Exists(Path.Combine(r.StagingDir, "Reborn-Server-Update-1.0.3.zip")));

			// The updater runs from its own copy in %TEMP%, so it can replace the installed one.
			string copy = Assert.Single(Directory.GetFiles(r.TempDir));
			Assert.Matches(@"^RebornUpdater-[0-9a-f]{32}\.exe$", Path.GetFileName(copy));
			Assert.Equal("the new updater", File.ReadAllText(copy));

			var start = Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal(copy, start.Exe);
			Assert.False(start.Minimized);   // the owner can watch it work
			Assert.Equal($"install --staged \"{staged}\" --version 1.0.3 --win64 \"{r.T.Win64}\" --panel-pid 4242 --panel \"{r.T.App.ExePath}\"",
				start.Args);
			Assert.True(Path.IsPathRooted(staged) && Path.IsPathRooted(r.T.Win64) && Path.IsPathRooted(r.T.App.ExePath));

			// Then the panel closes, so the updater can go ahead.
			Assert.Equal(1, r.T.App.Closed);

			// The next panel start deletes that copy (about 70 MB), and nothing else in %TEMP%.
			File.WriteAllText(Path.Combine(r.TempDir, "RebornUpdater-notes.exe"), "not ours");
			File.WriteAllText(Path.Combine(r.TempDir, "other.txt"), "not ours");
			_ = new UpdatesViewModel(r.Service, r.T.Dialogs, r.T.App, () => 0, r.Clock);
			Assert.False(File.Exists(copy));
			Assert.Equal(2, Directory.GetFiles(r.TempDir).Length);

			// If the updater won't start, the panel stays open and nothing is left behind.
			using var r2 = new UpdateRig();
			r2.Deployed("1.0.2\n");
			r2.Publish("1.0.3");
			await r2.Vm.CheckCommand.ExecuteAsync(null);
			r2.T.Runner.StartWorks = false;
			await r2.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(0, r2.T.App.Closed);
			Assert.Equal(UpdatesViewModel.UpdaterWouldNotStartText, r2.Vm.Status);
			Assert.Empty(Directory.GetFiles(r2.TempDir));
			Assert.False(Directory.Exists(r2.StagingDir));

			// A zip without the updater in it is not installed.
			using var r3 = new UpdateRig();
			r3.Deployed("1.0.2\n");
			using (var ms = new MemoryStream())
			{
				using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, leaveOpen: true))
					using (var w = new StreamWriter(zip.CreateEntry("Win64/reborn.dll").Open())) w.Write("mod");
				r3.Publish("1.0.3", zip: ms.ToArray());
			}
			await r3.Vm.CheckCommand.ExecuteAsync(null);
			await r3.Vm.UpdateNowCommand.ExecuteAsync(null);
			Assert.Equal(UpdatesViewModel.NoUpdaterText, r3.Vm.Status);
			Assert.Empty(r3.T.Runner.StartedWith);
			Assert.Equal(0, r3.T.App.Closed);
			Assert.False(Directory.Exists(r3.StagingDir));
		}

		private static void Keep(UpdateRig r, string version)
		{
			string dir = Path.Combine(r.T.Win64, "RebornVersions", version);
			Directory.CreateDirectory(dir);
			File.WriteAllText(Path.Combine(dir, Installer.BackupMarkerFile), version + "\n");
		}

		[Fact]
		public async Task roll_back_needs_confirmation()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			Keep(r, "1.0.1");
			File.WriteAllText(Path.Combine(r.T.Win64, "ControlPanel", "RebornUpdater.exe"), "the installed updater");
			r.Vm.Reload();
			Assert.Equal(new[] { "1.0.1" }, r.Vm.Kept);

			r.T.Dialogs.ConfirmAnswer = false;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			Assert.Equal(new[] { "Go back to 1.0.1? The server restarts." }, r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.Equal(0, r.T.App.Closed);
			Assert.Empty(Directory.GetFiles(r.TempDir));

			// A version that isn't kept is not even asked about.
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.9");
			Assert.Single(r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);

			r.T.Dialogs.ConfirmAnswer = true;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			string copy = Assert.Single(Directory.GetFiles(r.TempDir));
			Assert.Matches(@"^RebornUpdater-[0-9a-f]{32}\.exe$", Path.GetFileName(copy));
			Assert.Equal("the installed updater", File.ReadAllText(copy));
			var start = Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal(copy, start.Exe);
			Assert.Equal($"rollback --to \"1.0.1\" --win64 \"{r.T.Win64}\" --panel-pid 4242 --panel \"{r.T.App.ExePath}\"", start.Args);
			Assert.Equal(1, r.T.App.Closed);
		}

		[Fact]
		public async Task roll_back_refuses_unsafe_version_names()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			Keep(r, "1.0.1");
			File.WriteAllText(Path.Combine(r.T.Win64, "ControlPanel", "RebornUpdater.exe"), "the installed updater");
			r.T.Dialogs.ConfirmAnswer = true;

			// A name that would add its own arguments to the updater's command line.
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1\" --win64 \"C:\\elsewhere");
			Assert.Empty(r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);

			// An unfinished-update mark naming such a version reads as "unknown", as the Installer reads it.
			File.WriteAllText(Path.Combine(r.T.Win64, "RebornVersions", Installer.InProgressFile),
				JsonSerializer.Serialize(new { from = "..\\..\\x\" --to \"1.0.1", to = "1.0.3\" --evil", when = "2026-10-26T10:00:00Z" }));
			r.Vm.Reload();
			Assert.Equal("unknown", r.Vm.Unfinished);
			Assert.False(r.Vm.UnfinishedCanRollBack);
			Assert.Equal(Installer.StuckMessage(Path.Combine(r.T.Win64, "RebornVersions", Installer.InProgressFile)), r.Vm.UnfinishedText);
			await r.Vm.RollBackCommand.ExecuteAsync("..\\..\\x\" --to \"1.0.1");
			Assert.Empty(r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);
		}

		[Fact]
		public async Task a_timed_check_during_update_now_never_lets_a_second_one_start()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			r.Players = 2;
			r.T.Dialogs.ConfirmAnswer = true;

			// While the question is open, the refresh timer ticks with the next check due.
			Task? timed = null;
			r.T.Dialogs.WhileAsking = _ =>
			{
				r.T.Dialogs.WhileAsking = null;
				r.Clock.Now += TimeSpan.FromHours(6);
				timed = r.Vm.Tick();
			};
			// The download waits until the test lets it go (at most a few seconds).
			using var gate = new SemaphoreSlim(0);
			var answer = r.T.Runner.OnRun!;
			r.T.Runner.OnRun = (f, a) =>
			{
				if (a.StartsWith("release download 1.0.3")) gate.Wait(TimeSpan.FromSeconds(5));
				return answer(f, a);
			};

			Task first = r.Vm.UpdateNowCommand.ExecuteAsync(null);
			if (timed != null) await timed;   // a check that slipped in has finished
			Assert.True(r.Vm.Busy);
			Assert.Null(r.Vm.Tick());
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);   // a second press does nothing
			Assert.Single(r.T.Dialogs.Questions);

			gate.Release();
			await first;
			Assert.Single(r.T.Runner.Ran, c => c.Contains("release download 1.0.3"));
			Assert.Equal("the new mod", File.ReadAllText(Path.Combine(r.StagingDir, "1.0.3", "Win64", "reborn.dll")));
			Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal(1, r.T.App.Closed);
		}

		[Fact]
		public async Task update_now_rechecks_the_installed_version_first()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal("1.0.3", r.Vm.Available);

			// Meanwhile 1.0.3 was installed another way (tools.vm update).
			r.Deployed("1.0.3\n");
			int ran = r.T.Runner.Ran.Count;
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);

			Assert.Equal("Already up to date.", r.Vm.Status);
			Assert.Equal(ran, r.T.Runner.Ran.Count);
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.Equal(0, r.T.App.Closed);
			Assert.Null(r.Vm.Available);
			Assert.False(Directory.Exists(r.StagingDir));
			Assert.False(r.Vm.Busy);
		}

		[Fact]
		public void programs_are_found_only_in_absolute_path_folders()
		{
			using var t = new TempPanel();
			string bin = Path.Combine(t.Root, "bin");
			Directory.CreateDirectory(bin);
			string gh = Path.Combine(bin, "gh.cmd");
			File.WriteAllText(gh, "@echo off");

			Assert.Equal(gh, ProgramPath.Find("gh", "relative-folder;" + bin, ".EXE;.CMD"), ignoreCase: true);   // "gh.CMD" is the same file
			Assert.Equal(gh, ProgramPath.Find("gh.cmd", bin, ".EXE"));
			Assert.Equal(gh, ProgramPath.Find(gh, "", ".EXE"));

			// Not found: null (never Windows' own search, which looks next to the panel first).
			Assert.Null(ProgramPath.Find("gh", bin, ".EXE"));
			Assert.Null(ProgramPath.Find("gh", "", ".EXE;.CMD"));
			Assert.Null(ProgramPath.Find(Path.Combine(bin, "missing.exe"), bin, ".EXE"));
			Assert.Null(ProgramPath.Find("bin\\gh.cmd", t.Root, ".CMD"));

			// A relative PATH entry is never searched, even when it leads to the program.
			string relative = Path.GetRelativePath(Environment.CurrentDirectory, bin);
			Assert.False(Path.IsPathRooted(relative));
			Assert.Null(ProgramPath.Find("gh", relative, ".CMD"));
		}

		[Fact]
		public async Task unfinished_update_offers_only_its_roll_back()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.3\n");
			Keep(r, "1.0.2");
			Keep(r, "1.0.1");
			File.WriteAllText(Path.Combine(r.T.Win64, "ControlPanel", "RebornUpdater.exe"), "the installed updater");
			File.WriteAllText(Path.Combine(r.T.Win64, "RebornVersions", Installer.InProgressFile),
				"{\"from\":\"1.0.2\",\"to\":\"1.0.3\",\"when\":\"2026-10-26T10:00:00Z\"}");
			r.Publish("1.0.4");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			r.Vm.Reload();

			Assert.Equal("1.0.2", r.Vm.Unfinished);
			Assert.Equal("The update to 1.0.3 didn't finish, so the server may be a mix of two versions. Roll back to 1.0.2 to put it right.",
				r.Vm.UnfinishedText);
			Assert.False(r.Vm.UpdateNowCommand.CanExecute(null));
			Assert.Empty(r.Vm.Kept);   // the one way out is the button in the notice

			r.T.Dialogs.ConfirmAnswer = true;
			await r.Vm.UpdateNowCommand.ExecuteAsync(null);
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			Assert.Empty(r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);

			// The header doesn't offer the update while this is unfinished.
			MainViewModel main = r.T.Main(r.Clock, r.Service);
			await main.RefreshAsync();
			await main.Updates.CheckTask!;
			Assert.Equal("1.0.4", main.Updates.Available);
			Assert.Equal("", main.UpdateLine);

			// The installed updater may be half-copied: the one from the update that broke off is used.
			string stagedUpdater = Path.Combine(r.StagingDir, "1.0.3", "Win64", "ControlPanel", "RebornUpdater.exe");
			Directory.CreateDirectory(Path.GetDirectoryName(stagedUpdater)!);
			File.WriteAllText(stagedUpdater, "the staged updater");

			await r.Vm.RollBackCommand.ExecuteAsync("1.0.2");
			Assert.Equal(new[] { "Go back to 1.0.2? The server restarts." }, r.T.Dialogs.Questions);
			var start = Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal("rollback --to \"1.0.2\" --win64 \"" + r.T.Win64 + "\" --panel-pid 4242 --panel \"" + r.T.App.ExePath + "\"",
				start.Args);
			Assert.Equal("the staged updater", File.ReadAllText(start.Exe));

			// A mark that can't be read still says plainly what happened, and what to do instead.
			string mark = Path.Combine(r.T.Win64, "RebornVersions", Installer.InProgressFile);
			File.WriteAllText(mark, "{ broken");
			r.Vm.Reload();
			Assert.Equal("unknown", r.Vm.Unfinished);
			Assert.False(r.Vm.UnfinishedCanRollBack);
			Assert.Equal("An earlier update did not finish and its saved copy is gone. Delete " + mark + " and check the server by hand.",
				r.Vm.UnfinishedText);
		}

		[Fact]
		public async Task unfinished_update_whose_saved_copy_is_gone_offers_no_roll_back()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.3\n");
			File.WriteAllText(Path.Combine(r.T.Win64, "ControlPanel", "RebornUpdater.exe"), "the installed updater");
			string mark = Path.Combine(Directory.CreateDirectory(Path.Combine(r.T.Win64, "RebornVersions")).FullName, Installer.InProgressFile);
			File.WriteAllText(mark, "{\"from\":\"1.0.2\",\"to\":\"1.0.3\",\"when\":\"2026-10-26T10:00:00Z\"}");
			r.Vm.Reload();

			Assert.Equal("1.0.2", r.Vm.Unfinished);
			Assert.False(r.Vm.UnfinishedCanRollBack);
			Assert.Equal(Installer.StuckMessage(mark), r.Vm.UnfinishedText);
			Assert.False(r.Vm.UpdateNowCommand.CanExecute(null));

			r.T.Dialogs.ConfirmAnswer = true;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.2");
			Assert.Empty(r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.Equal(0, r.T.App.Closed);
			Assert.Equal(Installer.StuckMessage(mark), r.Vm.Status);

			// Once the saved copy is there again, the notice offers its roll back.
			Keep(r, "1.0.2");
			r.Vm.Reload();
			Assert.True(r.Vm.UnfinishedCanRollBack);
		}

		[Fact]
		public async Task roll_back_warns_when_players_are_in_games()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			Keep(r, "1.0.1");
			File.WriteAllText(Path.Combine(r.T.Win64, "ControlPanel", "RebornUpdater.exe"), "the installed updater");
			r.Vm.Reload();

			r.Players = 2;
			r.T.Dialogs.ConfirmAnswer = false;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			Assert.Equal(new[] { "2 players are in games. Going back stops those games. Go back to 1.0.1?" }, r.T.Dialogs.Questions);
			Assert.Empty(r.T.Runner.StartedWith);
			Assert.False(r.Vm.Busy);

			r.Players = 1;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			Assert.Equal("1 player is in a game. Going back stops that game. Go back to 1.0.1?", r.T.Dialogs.Questions[1]);

			r.T.Dialogs.ConfirmAnswer = true;
			await r.Vm.RollBackCommand.ExecuteAsync("1.0.1");
			Assert.Single(r.T.Runner.StartedWith);
			Assert.Equal(1, r.T.App.Closed);
		}

		[Fact]
		public async Task already_current_is_found_before_asking_about_players()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			await r.Vm.CheckCommand.ExecuteAsync(null);
			r.Deployed("1.0.3\n");   // installed another way meanwhile
			r.Players = 2;

			await r.Vm.UpdateNowCommand.ExecuteAsync(null);

			Assert.Empty(r.T.Dialogs.Questions);
			Assert.Equal(UpdatesViewModel.AlreadyUpToDateText, r.Vm.Status);
			Assert.Null(r.Vm.Available);
		}

		[Fact]
		public async Task a_check_whose_reload_fails_still_ends()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");
			File.WriteAllText(Path.Combine(Directory.CreateDirectory(Path.Combine(r.T.Win64, "RebornVersions")).FullName, Installer.LastUpdateFile),
				"{\"version\":\"1.0.2\",\"ok\":true,\"message\":\"Updated to 1.0.2.\",\"details\":\"\",\"when\":\"2026-10-26T11:30:00Z\"}");
			r.Clock.ZoneThrows = true;

			await r.Vm.CheckCommand.ExecuteAsync(null);

			Assert.False(r.Vm.Busy);
			Assert.StartsWith("The update check hit a problem", r.Vm.Status);
			Assert.True(r.Vm.CheckCommand.CanExecute(null));
			r.Clock.ZoneThrows = false;
			await r.Vm.CheckCommand.ExecuteAsync(null);
			Assert.Equal("1.0.3", r.Vm.Available);
		}

		[Fact]
		public async Task last_result_is_shown()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			MainViewModel main = r.T.Main(r.Clock, r.Service);
			await main.RefreshAsync();
			Assert.Null(main.Updates.LastResult);

			File.WriteAllText(Path.Combine(Directory.CreateDirectory(Path.Combine(r.T.Win64, "RebornVersions")).FullName, Installer.LastUpdateFile),
				JsonSerializer.Serialize(new
				{
					version = "1.0.3",
					ok = false,
					message = "The update to 1.0.3 didn't work: no game showed up within 3 minutes. 1.0.2 was put back and matchmaking is running again.",
					details = "First error: timed out",
					when = "2026-10-26T10:00:00Z",
				}));
			await main.RefreshAsync();
			Assert.Equal("2026-10-26 10:00: The update to 1.0.3 didn't work: no game showed up within 3 minutes. 1.0.2 was put back and matchmaking is running again.",
				main.Updates.LastResult);
			Assert.False(main.Updates.LastResultOk);
			Assert.Equal("First error: timed out", main.Updates.LastResultDetails);

			File.WriteAllText(Path.Combine(r.T.Win64, "RebornVersions", Installer.LastUpdateFile),
				"{\"version\":\"1.0.3\",\"ok\":true,\"message\":\"Updated to 1.0.3. A game is up and running.\",\"details\":\"\",\"when\":\"2026-10-26T11:30:00Z\"}");
			r.Vm.Reload();
			Assert.Equal("2026-10-26 11:30: Updated to 1.0.3. A game is up and running.", r.Vm.LastResult);
			Assert.True(r.Vm.LastResultOk);
			Assert.Null(r.Vm.LastResultDetails);

			// A damaged file is not an error, just nothing to show.
			File.WriteAllText(Path.Combine(r.T.Win64, "RebornVersions", Installer.LastUpdateFile), "not json");
			r.Vm.Reload();
			Assert.Null(r.Vm.LastResult);
		}

		[Fact]
		public async Task checks_at_launch_and_every_six_hours()
		{
			using var r = new UpdateRig();
			r.Deployed("1.0.2\n");
			r.Publish("1.0.3");

			Task? first = r.Vm.Tick();
			Assert.NotNull(first);
			await first!;
			Assert.Single(r.T.Runner.Ran);
			Assert.Equal("1.0.3", r.Vm.Available);

			r.Clock.Now += TimeSpan.FromHours(5);
			Assert.Null(r.Vm.Tick());
			Assert.Single(r.T.Runner.Ran);

			// Due again, but a check still running is never doubled up.
			r.Clock.Now += TimeSpan.FromHours(1);
			using var gate = new SemaphoreSlim(0);
			var answer = r.T.Runner.OnRun!;
			r.T.Runner.OnRun = (f, a) => { gate.Wait(); return answer(f, a); };
			Task? second = r.Vm.Tick();
			Assert.NotNull(second);
			Assert.True(r.Vm.Busy);
			Assert.Null(r.Vm.Tick());
			await r.Vm.CheckCommand.ExecuteAsync(null);   // the button does nothing while one runs
			gate.Release();
			await second!;
			Assert.Equal(2, r.T.Runner.Ran.Count);
			Assert.False(r.Vm.Busy);
		}
	}
}
