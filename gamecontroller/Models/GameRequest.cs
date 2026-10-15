namespace gamecontroller.Models
{
	// What the game sends to POST /api/games/start (GameCoordinator.cpp StartGame).
	public class GameRequest
	{
		public string Name { get; set; } = "";          // shown in the server browser
		public string MapMode { get; set; } = "";       // display name, e.g. "Story - The Algorithm"
		public string MapCommand { get; set; } = "";    // "open Caverns_P"; bots are appended here
		public int Players { get; set; } = 1;           // humans the match waits for
		public int BotsTeamA { get; set; }
		public int BotsTeamB { get; set; }
		public string PlayerName { get; set; } = "";    // the requester's display name, for the log
	}

	// appsettings.json "RequestedGame": when an on-request server is shut down.
	public record RequestedGameSettings(int NoJoinSeconds = 180, int EmptySeconds = 60);

	public record StartResult(bool Ok, string? Guid, string Error)
	{
		public static StartResult Started(string guid) => new(true, guid, "");
		public static StartResult Refused(string error) => new(false, null, error);
	}
}
