using System.Security.Cryptography;
using System.Text;

namespace ControlPanel.Core
{
	// The read-only GitHub token a fresh server uses to reach the private builds repo when gh isn't there
	// (or isn't signed in). Kept for this Windows user only, scrambled with Windows' own data protection:
	// %LOCALAPPDATA%\Reborn\builds-token.dat. It is never logged, shown or put on a command line.
	public static class GitHubToken
	{
		public const string FileName = "builds-token.dat";
		public const string SaveSwitch = "--save-github-token";

		// A token written by another program (or another user) can't be unscrambled without this.
		private static readonly byte[] Entropy = Encoding.UTF8.GetBytes("Battleborn Reborn builds token");

		public static string DefaultPath => Path.Combine(
			Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Reborn", FileName);

		// Keeps the token (trimmed). An empty token keeps nothing and leaves an earlier one alone.
		// path: tests give their own; the real one is DefaultPath. False when it could not be kept.
		public static bool Save(string? token, string? path = null)
		{
			string trimmed = (token ?? "").Trim();
			if (trimmed.Length == 0 || !OperatingSystem.IsWindows()) return false;
			try
			{
				path ??= DefaultPath;
				Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
				byte[] scrambled = ProtectedData.Protect(Encoding.UTF8.GetBytes(trimmed), Entropy, DataProtectionScope.CurrentUser);
				File.WriteAllBytes(path, scrambled);
				return true;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or CryptographicException) { return false; }
		}

		// The kept token, or null when there is none or it can't be read (another user's, damaged).
		public static string? Load(string? path = null)
		{
			if (!OperatingSystem.IsWindows()) return null;
			try
			{
				byte[] scrambled = File.ReadAllBytes(path ?? DefaultPath);
				string token = Encoding.UTF8.GetString(ProtectedData.Unprotect(scrambled, Entropy, DataProtectionScope.CurrentUser)).Trim();
				return token.Length == 0 ? null : token;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or CryptographicException) { return null; }
		}

		// ControlPanel.exe --save-github-token <file>: reads the token from the file, deletes the file
		// (always, even when nothing could be kept), keeps the token. Exit code 0 when the file was read and
		// the token kept, or when the file held no token (nothing to keep); 1 when it could not be done.
		public static int SaveFromFile(string file, string? savePath = null)
		{
			string text;
			try { text = File.ReadAllText(file); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
			{
				DeleteQuietly(file);
				return 1;
			}
			DeleteQuietly(file);
			if (text.Trim().Length == 0) return 0;
			return Save(text, savePath) ? 0 : 1;
		}

		// The command line's token switch: the file after it, or null when this isn't that command.
		public static string? SwitchFile(string[] args)
		{
			for (int i = 0; i < args.Length; i++)
				if (string.Equals(args[i], SaveSwitch, StringComparison.OrdinalIgnoreCase))
					return i + 1 < args.Length ? args[i + 1] : "";
			return null;
		}

		private static void DeleteQuietly(string file)
		{
			try { File.Delete(file); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { }
		}
	}
}
