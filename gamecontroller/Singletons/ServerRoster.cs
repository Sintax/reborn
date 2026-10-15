using gamecontroller.Models;

namespace gamecontroller.Singletons
{
	// One game server the coordinator keeps running, as written in appsettings.json.
	// "RandomHero": true makes that game server give every player a random hero, whatever they pick.
	public record RosterSlot(string Name, string MapMode, string ServerStartupCommand, int MaxNumPlayers, bool RandomHero = false);

	// Starts and watches Serverborn.exe processes; swapped for a fake in tests.
	public interface IServerLauncher
	{
		// instanceName is the server's -rbinstance (its own loader folder and log name).
		int Launch(string instanceName, string commandLine);
		bool IsRunning(int pid);
		void Kill(int pid);
	}

	// Keeps one live game server per roster slot and answers the server browser.
	public class ServerRoster
	{
		// A server that has not checked in for this long is assumed hung and gets replaced.
		public static readonly TimeSpan CheckInTimeout = TimeSpan.FromSeconds(20);
		// ...but a fresh server first needs this long to load its map before it can check in.
		public static readonly TimeSpan StartupGrace = TimeSpan.FromSeconds(120);

		private readonly IReadOnlyList<RosterSlot> _slots;
		private readonly IServerLauncher _launcher;
		private readonly IReadOnlyList<int> _ports;
		private readonly string _coordinatorBase;
		private readonly List<GameInstance> _instances = new();
		private readonly object _lock = new();
		private readonly RequestedGameSettings _requested;
		private static readonly System.Text.RegularExpressions.Regex MapCommandShape = new(@"^open [A-Za-z0-9_]+\z");

		// Set once the public address is known; nothing launches before then.
		public string? PublicIp { get; set; }

		public IReadOnlyList<GameInstance> Instances { get { lock (_lock) return _instances.ToList(); } }

		public ServerRoster(IReadOnlyList<RosterSlot> slots, IServerLauncher launcher, IReadOnlyList<int> ports, string coordinatorBase,
			RequestedGameSettings? requested = null)
		{
			_slots = slots;
			_launcher = launcher;
			_ports = ports;
			_coordinatorBase = coordinatorBase;
			_requested = requested ?? new RequestedGameSettings();
		}

		public static bool IsLateJoinMode(string mapMode) =>
			mapMode.StartsWith("Story", StringComparison.Ordinal) || mapMode.StartsWith("Operations", StringComparison.Ordinal);

		public static string InstanceNameFor(RosterSlot slot) => "rb-" + new string(slot.Name.ToLowerInvariant().Where(c => char.IsLetterOrDigit(c) || c == '-').ToArray());

		public void Reconcile(DateTime now)
		{
			if (PublicIp == null) return;

			lock (_lock)
			{
				foreach (GameInstance dead in _instances.Where(i => !Alive(i, now)).ToList())
				{
					if (_launcher.IsRunning(dead.PID)) _launcher.Kill(dead.PID);
					_instances.Remove(dead);
				}

				foreach (RosterSlot slot in _slots)
				{
					if (_instances.Any(i => i.Slot == slot)) continue;

					int port = _ports.FirstOrDefault(p => !_instances.Any(i => i.Port == p));
					if (port == 0) break;

					var instance = new GameInstance
					{
						Slot = slot,
						Port = port,
						CreationTime = now,
						LastServerCheckIn = now,
						Config = new GameCreationConfig(slot.Name, slot.MapMode, slot.ServerStartupCommand, slot.MaxNumPlayers),
						ConnectionString = "open " + PublicIp + ":" + port,
					};
					instance.PID = _launcher.Launch(InstanceNameFor(slot), CommandLine(InstanceNameFor(slot), instance));
					_instances.Add(instance);
				}
			}
		}

		private string CommandLine(string instanceName, GameInstance instance) =>
			"-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbinstance=" + instanceName
			+ " -rbcoordinator=" + _coordinatorBase + " -rbcoordinatorkey=" + instance.MyGuid
			+ (instance.Slot?.RandomHero == true ? " -rbrandomhero" : "");

		// A game a player asked for from the server browser. It is never relaunched; Alive() retires it.
		public StartResult StartRequested(GameRequest req, string keyLabel, DateTime now)
		{
			if (!MapCommandShape.IsMatch(req.MapCommand ?? "")) return StartResult.Refused("That map command is not allowed.");
			bool lateJoin = IsLateJoinMode(req.MapMode ?? "");
			int maxPlayers = lateJoin ? 5 : 10;
			if (req.Players < 1 || req.Players > maxPlayers) return StartResult.Refused($"Players must be 1 to {maxPlayers}.");
			if (req.BotsTeamA < 0 || req.BotsTeamA > 5 || req.BotsTeamB < 0 || req.BotsTeamB > 5) return StartResult.Refused("Bots per team must be 0 to 5.");
			if (PublicIp == null) return StartResult.Refused("The server is still finding its public address. Try again in a moment.");

			string command = req.MapCommand!;
			if (!lateJoin && req.BotsTeamA + req.BotsTeamB > 0)
				command += "?SpawnBotsTeamA=" + req.BotsTeamA + "?SpawnBotsTeamB=" + req.BotsTeamB;

			lock (_lock)
			{
				int port = _ports.FirstOrDefault(p => !_instances.Any(i => i.Port == p));
				if (port == 0) return StartResult.Refused($"Server is full ({_ports.Count} of {_ports.Count} games running). Try again later.");

				var instance = new GameInstance
				{
					Slot = null,
					KeyLabel = keyLabel,
					Port = port,
					CreationTime = now,
					LastServerCheckIn = now,
					Config = new GameCreationConfig(req.Name, req.MapMode!, command, req.Players),
					ConnectionString = "open " + PublicIp + ":" + port,
				};
				string name = "rb-req-" + port;
				instance.PID = _launcher.Launch(name, CommandLine(name, instance));
				_instances.Add(instance);
				return StartResult.Started(instance.PublicId);
			}
		}

		public bool Cancel(string publicId)
		{
			lock (_lock)
			{
				GameInstance? i = _instances.FirstOrDefault(x => x.PublicId == publicId);
				if (i == null) return false;
				i.Finished = true;
				return true;
			}
		}

		private bool Alive(GameInstance i, DateTime now)
		{
			if (i.Finished) return false;
			if (!_launcher.IsRunning(i.PID)) return false;
			if (now - i.CreationTime < StartupGrace && !i.HasCheckedIn) return true;
			if (!i.HasCheckedIn) return false;
			if (i.Slot == null && i.FirstCheckIn != null)
			{
				if (!i.EverHadPlayers && now - i.FirstCheckIn.Value > TimeSpan.FromSeconds(_requested.NoJoinSeconds)) return false;
				if (i.EverHadPlayers && i.CurrentNumPlayers == 0 && i.LastNonEmpty != null
					&& now - i.LastNonEmpty.Value > TimeSpan.FromSeconds(_requested.EmptySeconds)) return false;
			}
			return now - i.LastServerCheckIn <= CheckInTimeout;
		}

		private GameInstance? Find(string token)
		{
			lock (_lock) return _instances.FirstOrDefault(i => i.MyGuid == token);
		}

		public GameCreationConfig? ConfigFor(string token) => Find(token)?.Config;

		public bool RecordPoll(string token, int connectedPlayers, bool humansHaveStarted, DateTime now)
		{
			GameInstance? i = Find(token);
			if (i == null) return false;
			i.CurrentNumPlayers = connectedPlayers;
			i.MatchStarted = humansHaveStarted;
			i.LastServerCheckIn = now;
			i.HasCheckedIn = true;
			i.FirstCheckIn ??= now;
			if (connectedPlayers > 0) { i.EverHadPlayers = true; i.LastNonEmpty = now; }
			return true;
		}

		public bool MarkFinished(string token)
		{
			GameInstance? i = Find(token);
			if (i == null) return false;
			i.Finished = true;
			return true;
		}

		public List<ServerBrowserEntry> BrowserEntries()
		{
			lock (_lock)
			{
				return _instances.Select(i => new ServerBrowserEntry
				{
					InstanceName = i.Config.InstanceName,
					HumanReadableInstanceMapMode = i.Config.HumanReadableInstanceMapMode,
					CurrentNumPlayers = i.CurrentNumPlayers,
					MaxNumPlayers = i.Config.MaxNumPlayers,
					ServerConnectString = i.ConnectionString,
					MatchStarted = i.MatchStarted,
					MyGuid = i.PublicId,
					Ready = i.HasCheckedIn,
					AllowLateJoin = IsLateJoinMode(i.Config.HumanReadableInstanceMapMode),
				}).ToList();
			}
		}
	}

	// What the game's server browser shows for one server (GameCoordinator.hpp ServerBrowserEntry).
	public class ServerBrowserEntry
	{
		public string InstanceName { get; set; } = "";
		public string HumanReadableInstanceMapMode { get; set; } = "";
		public int CurrentNumPlayers { get; set; }
		public int MaxNumPlayers { get; set; }
		public string ServerConnectString { get; set; } = "";
		public bool MatchStarted { get; set; }
		public string MyGuid { get; set; } = "";
		public bool Ready { get; set; }
		public bool AllowLateJoin { get; set; }
	}
}
