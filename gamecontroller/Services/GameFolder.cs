namespace gamecontroller.Services
{
	// Where the game's Binaries\Win64 folder is: the game server, the Steam loader files and
	// reborn-users.json all live there.
	public static class GameFolder
	{
		public static string Resolve(IConfiguration config)
		{
			// The server kit puts the service in Win64\matchmaking; appsettings.json "Win64" can say otherwise.
			string here = AppContext.BaseDirectory.TrimEnd('\\', '/');
			string parent = Path.GetDirectoryName(here) ?? here;
			// A JSON null in appsettings.json reads back as "", not null.
			string? configured = config["Win64"];
			return !string.IsNullOrEmpty(configured) ? configured
				: File.Exists(Path.Combine(here, "Battleborn.exe")) ? here : parent;
		}
	}
}
