using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace ControlPanel.Core.ViewModels
{
	// The Games page: every running game with its players and heroes.
	public sealed class GamesViewModel : ObservableObject
	{
		public ObservableCollection<GameRow> Games { get; } = new();

		// Shown when the list is empty.
		public string EmptyText => Games.Count == 0 ? "No games are running." : "";

		// Everyone in any game right now (for the restart warning).
		public int PlayersInGames => Games.Sum(g => g.Players.Count);

		// Rows are updated in place (matched by game id) so the window keeps its selection.
		public void Update(IReadOnlyList<GameInfo> games, DateTime nowUtc)
		{
			var wanted = games.Select(g => g.PublicId).ToHashSet();
			for (int i = Games.Count - 1; i >= 0; i--)
				if (!wanted.Contains(Games[i].PublicId)) Games.RemoveAt(i);
			for (int i = 0; i < games.Count; i++)
			{
				GameRow? row = Games.FirstOrDefault(r => r.PublicId == games[i].PublicId);
				if (row == null)
				{
					row = new GameRow(games[i].PublicId);
					Games.Insert(Math.Min(i, Games.Count), row);
				}
				else if (Games.IndexOf(row) != i) Games.Move(Games.IndexOf(row), i);
				row.Apply(games[i], nowUtc);
			}
			OnPropertyChanged(nameof(EmptyText));
			OnPropertyChanged(nameof(PlayersInGames));
		}

		public void Clear()
		{
			Games.Clear();
			OnPropertyChanged(nameof(EmptyText));
			OnPropertyChanged(nameof(PlayersInGames));
		}
	}

	public sealed class GameRow : ObservableObject
	{
		public GameRow(string publicId) => PublicId = publicId;

		public string PublicId { get; }

		private string _name = "";
		public string Name { get => _name; private set => SetProperty(ref _name, value); }

		private string _map = "";
		public string Map { get => _map; private set => SetProperty(ref _map, value); }

		// "Story", "Operations", or "Versus, ban and pick" (plus ", random heroes").
		private string _mode = "";
		public string Mode { get => _mode; private set => SetProperty(ref _mode, value); }

		private int _port;
		public int Port { get => _port; private set => SetProperty(ref _port, value); }

		private string _runningFor = "";
		public string RunningFor { get => _runningFor; private set => SetProperty(ref _runningFor, value); }

		// Who started it ("" for the always-on games from the settings).
		private string _startedBy = "";
		public string StartedBy { get => _startedBy; private set => SetProperty(ref _startedBy, value); }

		// "Playing" once the match has begun, else "Waiting for players".
		private string _state = "";
		public string State { get => _state; private set => SetProperty(ref _state, value); }

		public ObservableCollection<PlayerRow> Players { get; } = new();

		internal void Apply(GameInfo g, DateTime nowUtc)
		{
			Name = g.Name;
			Map = g.MapMode;
			Mode = ModeText(g);
			Port = g.Port;
			RunningFor = g.StartedUtc == default ? "" : Display.Duration(nowUtc - Display.Utc(g.StartedUtc));
			StartedBy = g.StartedBy ?? "";
			State = g.MatchStarted ? "Playing" : "Waiting for players";
			bool versus = IsVersus(g);
			var players = (g.Players ?? Array.Empty<PlayerInfo>()).Select(p => new PlayerRow(p, versus)).ToList();
			if (!players.SequenceEqual(Players))
			{
				Players.Clear();
				foreach (PlayerRow p in players) Players.Add(p);
			}
		}

		private static bool IsStory(GameInfo g) => g.MapMode.StartsWith("Story", StringComparison.Ordinal);
		private static bool IsOperations(GameInfo g) => g.MapMode.StartsWith("Operations", StringComparison.Ordinal);

		// Two teams play each other only in Versus; in Story and Operations everyone is on one team.
		private static bool IsVersus(GameInfo g) => !IsStory(g) && !IsOperations(g);

		private static string ModeText(GameInfo g)
		{
			string random = g.RandomHero ? ", random heroes" : "";
			if (IsStory(g)) return "Story" + random;
			if (IsOperations(g)) return "Operations" + random;
			return "Versus, " + DraftText(g.DraftStyle) + random;
		}

		// The game's draft-style values (gamecontroller/Models/DraftStyles.cs) in plain words.
		private static string DraftText(string value) => value switch
		{
			"" or "QuickSelect" => "quick select",
			_ when value.StartsWith("BanPick", StringComparison.Ordinal) => "ban and pick",
			_ when value.StartsWith("Captains", StringComparison.Ordinal) => "captain's draft",
			"ChaosRumble" => "chaos rumble",
			"Classic" => "classic draft",
			_ => value,
		};
	}

	// One player in a game: their name, hero ("choosing a hero" until picked) and team. The team is
	// shown only in Versus (counted from 1, as players see it), and only once the server knows it.
	public sealed record PlayerRow(string Name, string Hero, string Team)
	{
		public PlayerRow(PlayerInfo p, bool versus) : this(p.Name, p.Hero == "" ? "choosing a hero" : p.Hero,
			versus && p.Team >= 0 ? $"Team {p.Team + 1}" : "") { }
	}
}
