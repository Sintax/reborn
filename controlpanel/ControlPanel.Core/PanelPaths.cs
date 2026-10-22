using System.Text.Json;

namespace ControlPanel.Core
{
	// Where everything the panel touches lives. The panel sits in <Win64>\ControlPanel\ (or right in
	// the game's Binaries\Win64 folder); matchmaking is in <Win64>\matchmaking\.
	public sealed class PanelPaths
	{
		public const int DefaultCoordinatorPort = 5000;

		// exeFolder: the folder the panel's exe is in. userProfile: %USERPROFILE%.
		public PanelPaths(string exeFolder, string userProfile)
		{
			string folder = Path.GetFullPath(exeFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
			Win64 = File.Exists(Path.Combine(folder, "Battleborn.exe")) ? folder : Path.GetDirectoryName(folder) ?? folder;
			string documents = Path.Combine(userProfile, "Documents");
			ZipOutputDir = Path.Combine(documents, "Reborn Friend Zips");
			GameLogsDir = Path.Combine(documents, "RebornLogs");
		}

		public static PanelPaths ForThisApp() =>
			new(AppContext.BaseDirectory, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));

		public string Win64 { get; }
		public string MatchmakingDir => Path.Combine(Win64, "matchmaking");
		public string MatchmakingExe => Path.Combine(MatchmakingDir, "gamecontroller.exe");
		public string MatchmakingSettings => Path.Combine(MatchmakingDir, "appsettings.json");
		public string MatchmakingLog => Path.Combine(MatchmakingDir, "matchmaking.log");
		public string AdminTokenFile => Path.Combine(MatchmakingDir, "admin-token.txt");
		public string GameExe => Path.Combine(Win64, "Battleborn.exe");
		public string ServerExe => Path.Combine(Win64, "Serverborn.exe");
		public string PlayerInstallerZip => Path.Combine(Win64, "reborn-player-installer.zip");
		public string ZipRecordFile => Path.Combine(Win64, "reborn-zips.json");
		public string ZipOutputDir { get; }
		public string GameLogsDir { get; }

		// The port matchmaking answers on: "CoordinatorPort" in matchmaking's appsettings.json
		// (a number or a string), else 5000.
		public int CoordinatorPort
		{
			get
			{
				try
				{
					using var doc = JsonDocument.Parse(File.ReadAllBytes(MatchmakingSettings),
						new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
					if (doc.RootElement.ValueKind == JsonValueKind.Object
						&& doc.RootElement.TryGetProperty("CoordinatorPort", out JsonElement value))
					{
						if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int n) && n is > 0 and <= 65535) return n;
						if (value.ValueKind == JsonValueKind.String && int.TryParse(value.GetString(), out int s) && s is > 0 and <= 65535) return s;
					}
				}
				catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
				return DefaultCoordinatorPort;
			}
		}

		// The admin API on this machine only.
		public Uri AdminBaseUri => new($"http://127.0.0.1:{CoordinatorPort}/api/admin/");

		// The token matchmaking wrote, or null if it isn't there (yet). Read on every call, so a token
		// made by a fresh matchmaking is picked up without restarting the panel.
		public string? ReadToken()
		{
			try
			{
				string token = File.ReadAllText(AdminTokenFile).Trim();
				return token.Length == 0 ? null : token;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
		}
	}
}
