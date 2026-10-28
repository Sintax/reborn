using System.Diagnostics;
using System.IO.Compression;
using ControlPanel.Core;

namespace ControlPanel.Tests
{
	public class SevenZipTests
	{
		private const string Key = "ABCDEFGHJKMNPQRSTVWXYZ23";

		private static string BaseZip(string dir)
		{
			string path = Path.Combine(dir, "reborn-player-installer.zip");
			using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
			foreach (string name in new[] { "Install.bat", "README.txt", "files/reborn.dll" })
				using (var w = new StreamWriter(zip.CreateEntry(name).Open())) w.Write("x");
			return path;
		}

		[Fact]
		public void Find_needs_both_exe_and_sfx()
		{
			using var t = new TempPanel();
			string both = FakeRunner.FakeSevenZipFolder(t.Root);
			string exeOnly = Directory.CreateDirectory(Path.Combine(t.Root, "exe-only")).FullName;
			File.WriteAllText(Path.Combine(exeOnly, "7z.exe"), "");
			string sfxOnly = Directory.CreateDirectory(Path.Combine(t.Root, "sfx-only")).FullName;
			File.WriteAllText(Path.Combine(sfxOnly, "7z.sfx"), "");

			Assert.Null(SevenZip.Find(new[] { exeOnly, sfxOnly, Path.Combine(t.Root, "nowhere"), "" }));
			Assert.Equal(both, SevenZip.Find(new[] { exeOnly, both }));
		}

		[Fact]
		public void Missing_7zip_says_install_and_makes_nothing()
		{
			using var t = new TempPanel();
			string outDir = Path.Combine(t.Root, "out");
			string exeOnly = Directory.CreateDirectory(Path.Combine(t.Root, "exe-only")).FullName;
			File.WriteAllText(Path.Combine(exeOnly, "7z.exe"), "");   // 7z.sfx missing: counts as not installed
			var runner = new FakeRunner();

			var (ok, message, path) = FriendZip.Make(BaseZip(t.Root), "Kaz", Key, outDir, runner,
				findSevenZip: () => SevenZip.Find(new[] { exeOnly }));
			Assert.False(ok);
			Assert.Null(path);
			Assert.Equal("7-Zip is not installed on this server. Install 7-Zip from 7-zip.org, then try again.", message);
			Assert.Empty(runner.RanList);
			Assert.True(!Directory.Exists(outDir) || Directory.GetFileSystemEntries(outDir).Length == 0);
		}

		[Fact]
		public void Pack_passes_arguments_separately()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			string outDir = Path.Combine(t.Root, "out");
			var runner = new FakeRunner();
			runner.AcceptPacking();

			var (ok, message, path) = FriendZip.Make(BaseZip(t.Root), "A_b-9", Key, outDir, runner, sevenZipDir: sevenZip);
			Assert.True(ok, message);
			Assert.Equal(Path.Combine(outDir, "Reborn-Mod-Installer_A_b-9.exe"), path);

			var ran = Assert.Single(runner.RanList);
			Assert.Equal(Path.Combine(sevenZip, "7z.exe"), ran.File);
			Assert.Equal(SevenZip.PackLimit, ran.Limit);
			string[] a = ran.Arguments;
			Assert.Equal(new[] { "a", "-t7z", "-mx=7", "-sfx" + Path.Combine(sevenZip, "7z.sfx"), "-y" }, a.Take(5));
			Assert.EndsWith(".tmp", a[^2]);
			Assert.StartsWith(Path.Combine(outDir, "Reborn-Mod-Installer_A_b-9.exe"), a[^2]);
			Assert.EndsWith(Path.DirectorySeparatorChar + "*", a[^1]);
			Assert.DoesNotContain(a, x => x.StartsWith("-p") || x.StartsWith("-mhe"));
		}

		[Fact]
		public void Pack_failure_cleans_up_and_keeps_old_exe()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			string outDir = Path.Combine(t.Root, "out");
			Directory.CreateDirectory(outDir);
			string old = Path.Combine(outDir, "Reborn-Mod-Installer_Kaz.exe");
			File.WriteAllText(old, "older file");
			string record = Path.Combine(t.Root, "rec.json");
			var runner = new FakeRunner
			{
				OnRunList = (_, args) => { File.WriteAllText(args[^2], "half"); return (2, "Fatal error"); },
			};

			var (ok, message, path) = FriendZip.Make(BaseZip(t.Root), "Kaz", Key, outDir, runner, record, sevenZipDir: sevenZip);
			Assert.False(ok);
			Assert.Null(path);
			Assert.DoesNotContain(Key, message);
			Assert.Equal("older file", File.ReadAllText(old));
			Assert.Equal(new[] { old }, Directory.GetFileSystemEntries(outDir));   // no temp file, no staging folder
			Assert.False(File.Exists(record));
		}

		[Fact]
		public void Pack_timeout_kills_and_says_so()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			string outDir = Path.Combine(t.Root, "out");
			// The real runner kills the program and answers -1 when the limit passes.
			var runner = new FakeRunner { OnRunList = (_, _) => (-1, "It took longer than 5 minutes and was stopped.") };

			var (ok, message, path) = FriendZip.Make(BaseZip(t.Root), "Kaz", Key, outDir, runner, sevenZipDir: sevenZip);
			Assert.False(ok);
			Assert.Null(path);
			Assert.Contains("5 minutes", message);
			Assert.Empty(Directory.GetFileSystemEntries(outDir));
		}

		[Fact]
		public void Make_stages_the_same_files_as_the_old_zip()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			var staged = new SortedSet<string>(StringComparer.Ordinal);
			var runner = new FakeRunner();
			runner.AcceptPacking(args =>
			{
				string dir = Path.GetDirectoryName(args[^1])!;
				Assert.StartsWith(".staging-", Path.GetFileName(dir));
				foreach (string f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
					staged.Add(Path.GetRelativePath(dir, f).Replace('\\', '/'));
			});
			var (ok, message, _) = FriendZip.Make(BaseZip(t.Root), "Kaz", Key, Path.Combine(t.Root, "out"), runner, sevenZipDir: sevenZip);
			Assert.True(ok, message);
			Assert.Equal(new[] { "Install.bat", "README-Kaz.txt", "files/reborn-key.txt", "files/reborn-player.txt", "files/reborn.dll",
				"files/steam_settings/configs.user.ini" }, staged);
		}

		[Fact]
		public void Message_never_contains_key()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			var good = new FakeRunner();
			good.AcceptPacking();
			var (ok, message, _) = FriendZip.Make(BaseZip(t.Root), "Kaz", Key, Path.Combine(t.Root, "out"), good, sevenZipDir: sevenZip);
			Assert.True(ok);
			Assert.DoesNotContain(Key, message);
			Assert.Contains("Send it to Kaz only: it holds their key.", message);
			Assert.Contains("Kaz's file is ready: ", message);
		}

		[Fact]
		public void Old_zip_is_left_alone()
		{
			using var t = new TempPanel();
			string sevenZip = FakeRunner.FakeSevenZipFolder(t.Root);
			string outDir = Path.Combine(t.Root, "out");
			Directory.CreateDirectory(outDir);
			string oldZip = Path.Combine(outDir, "Reborn-Mod-Installer_Kaz.zip");
			File.WriteAllText(oldZip, "old zip");
			var runner = new FakeRunner();
			runner.AcceptPacking();
			Assert.True(FriendZip.Make(BaseZip(t.Root), "Kaz", Key, outDir, runner, sevenZipDir: sevenZip).ok);
			Assert.Equal("old zip", File.ReadAllText(oldZip));
		}

		[Fact]
		public void Labels_say_file_not_zip()
		{
			string xaml = File.ReadAllText(Path.Combine(LogicTests.RepoRoot(), "controlpanel", "ControlPanel", "Views", "UsersView.xaml"));
			foreach (string expected in new[] { "Make file", "File made", "Open files folder", "Files are saved in", "Last file made" })
				Assert.Contains(expected, xaml);
			foreach (string gone in new[] { "Make zip", "Zip made", "Open zip folder", "Zips are saved in", "Last zip made" })
				Assert.DoesNotContain(gone, xaml);
		}

		[Fact]
		public void Guide_first_step_is_the_exe()
		{
			string guide = File.ReadAllText(Path.Combine(LogicTests.RepoRoot(), "docs", "FRIEND-SETUP.md"));
			Assert.Contains("1. Double-click Reborn-Mod-Installer_{NAME}.exe, choose a folder and press Extract. If Windows says it protected your PC, press More info, then Run anyway. Then open that folder and continue below.", guide);
		}

		[Fact]
		public void Real_7zip_round_trip()
		{
			string? sevenZip = SevenZip.Find();
			if (sevenZip == null) return;   // 7-Zip is not installed here: nothing to check
			string dir = Path.Combine(Path.GetTempPath(), "bb-7z-" + Guid.NewGuid().ToString("N"));
			try
			{
				string src = Path.Combine(dir, "src");
				Directory.CreateDirectory(Path.Combine(src, "files"));
				File.WriteAllText(Path.Combine(src, "Install.bat"), "@echo off\r\n");
				File.WriteAllText(Path.Combine(src, "files", "reborn-key.txt"), "k\r\n");
				string exe = Path.Combine(dir, "out", "Reborn-Mod-Installer_Kaz.exe");

				var runner = new FakeRunner { OnRunList = (file, args) => RunReal(file, args) };
				var (ok, message) = SevenZip.PackSfx(sevenZip, src, exe, runner, TimeSpan.FromMinutes(1));
				Assert.True(ok, message);
				Assert.Equal(new[] { exe }, Directory.GetFiles(Path.GetDirectoryName(exe)!));   // no temp file left

				var (exit, listing) = RunReal(Path.Combine(sevenZip, "7z.exe"), new[] { "l", exe });
				Assert.Equal(0, exit);
				Assert.Contains("Install.bat", listing);
				Assert.Contains("reborn-key.txt", listing);
			}
			finally
			{
				try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
			}
		}

		private static (int, string) RunReal(string file, string[] args)
		{
			var info = new ProcessStartInfo(file) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
			foreach (string a in args) info.ArgumentList.Add(a);
			using var p = Process.Start(info)!;
			Task<string> err = p.StandardError.ReadToEndAsync();
			string output = p.StandardOutput.ReadToEnd();
			p.WaitForExit();
			return (p.ExitCode, output + err.Result);
		}
	}
}
