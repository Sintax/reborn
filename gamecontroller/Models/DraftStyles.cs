namespace gamecontroller.Models
{
	// How Versus players choose heroes. The names are what the game's menu sends to the coordinator;
	// GameValue turns one into the value the game server is started with (?DraftStyle=<value>).
	public static class DraftStyles
	{
		public const string QuickSelect = "QuickSelect";

		public static readonly string[] Names = { "QuickSelect", "BanPick", "CaptainsDraft", "ChaosRumble", "Classic" };

		// teamSize is players per side; 3 or fewer uses the game's 3v3 variant where one exists.
		// Returns null for a name that is not in Names.
		public static string? GameValue(string name, int teamSize)
		{
			string size = teamSize <= 3 ? "3v3" : "5v5";
			return name switch
			{
				"QuickSelect" => "QuickSelect",
				"BanPick" => "BanPick_" + size,
				"CaptainsDraft" => "Captains_" + size,
				"ChaosRumble" => "ChaosRumble",
				"Classic" => "Classic",
				_ => null,
			};
		}

		// Players per side for a game: everyone (people and bots) split over two teams, rounded up.
		public static int TeamSize(int people, int botsTeamA, int botsTeamB) => (people + botsTeamA + botsTeamB + 1) / 2;
	}
}
