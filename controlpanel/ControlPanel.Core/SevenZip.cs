using Microsoft.Win32;

namespace ControlPanel.Core
{
	// 7-Zip, used to pack a friend's files into one file that unpacks by double-click
	// (a 7z self-extracting .exe, no password).
	public static class SevenZip
	{
		public const string NotInstalled = "7-Zip is not installed on this server. Install 7-Zip from 7-zip.org, then try again.";

		// How long one 7-Zip run may take before it is stopped.
		public static readonly TimeSpan PackLimit = TimeSpan.FromMinutes(5);

		// The folder that holds both 7z.exe and 7z.sfx, or null. candidates: folders to try, in order; the
		// default is 7-Zip's registered folder (64-bit then 32-bit view), then C:\Program Files\7-Zip.
		public static string? Find(IEnumerable<string>? candidates = null)
		{
			foreach (string dir in candidates ?? DefaultFolders())
			{
				try
				{
					if (!string.IsNullOrWhiteSpace(dir) && File.Exists(Path.Combine(dir, "7z.exe")) && File.Exists(Path.Combine(dir, "7z.sfx")))
						return dir;
				}
				catch (ArgumentException) { }   // characters a path can't have
			}
			return null;
		}

		private static IEnumerable<string> DefaultFolders()
		{
			if (OperatingSystem.IsWindows())
			{
				foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
				{
					string? registered = null;
					try
					{
						using RegistryKey hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
						using RegistryKey? key = hklm.OpenSubKey(@"SOFTWARE\7-Zip");
						registered = key?.GetValue("Path") as string;
					}
					catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
					if (!string.IsNullOrWhiteSpace(registered)) yield return registered;
				}
			}
			yield return @"C:\Program Files\7-Zip";
		}

		// Packs everything in sourceDir into outExe: a self-unpacking 7z file (no password). Built as a
		// temp file next to outExe and moved over it at the end, so a failure never leaves a half-made
		// file or removes an older one. Arguments go to 7-Zip one by one (never through a shell).
		public static (bool ok, string message) PackSfx(string sevenZipDir, string sourceDir, string outExe,
			IProcessRunner runner, TimeSpan limit)
		{
			string tmp = outExe + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outExe))!);
				var arguments = new List<string>
				{
					"a", "-t7z", "-mx=7",
					"-sfx" + Path.Combine(sevenZipDir, "7z.sfx"),
					"-y",
					tmp,
					Path.Combine(sourceDir, "*"),
				};
				int exit = runner.Run(Path.Combine(sevenZipDir, "7z.exe"), arguments, limit, out _);
				if (exit != 0)
					return (false, exit == -1
						? $"7-Zip could not be run, or took longer than {(int)limit.TotalMinutes} minutes. Try again."
						: "7-Zip could not pack the files. Check that there is free disk space and the folder can be written to, then try again.");
				if (!File.Exists(tmp))
					return (false, "7-Zip finished but made no file. Try again.");
				File.Move(tmp, outExe, overwrite: true);
				return (true, "");
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return (false, $"The file could not be written in {Path.GetDirectoryName(outExe)}. Check that the folder can be written to and the file is not open, then try again.");
			}
			finally
			{
				try { if (File.Exists(tmp)) File.Delete(tmp); }
				catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
			}
		}
	}
}
