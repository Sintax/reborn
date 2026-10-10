using gamecontroller.Models;

namespace gamecontroller.Singletons
{
	// One game server the coordinator keeps running, as written in appsettings.json.
	public record RosterSlot(string Name, string MapMode, string ServerStartupCommand, int MaxNumPlayers);

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

		// Set once the public address is known; nothing launches before then.
		public string? PublicIp { get; set; }

		public IReadOnlyList<GameInstance> Instances { get { lock (_lock) return _instances.ToList(); } }

		public ServerRoster(IReadOnlyList<RosterSlot> slots, IServerLauncher launcher, IReadOnlyList<int> ports, string coordinatorBase)
		{
			_slots = slots;
			_launcher = launcher;
			_ports = ports;
			_coordinatorBase = coordinatorBase;
		}

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
					string commandLine = "-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbinstance=" + InstanceNameFor(slot)
						+ " -rbcoordinator=" + _coordinatorBase + " -rbcoordinatorkey=" + instance.MyGuid;
					instance.PID = _launcher.Launch(InstanceNameFor(slot), commandLine);
					_instances.Add(instance);
				}
			}
		}

		private bool Alive(GameInstance i, DateTime now)
		{
			if (i.Finished) return false;
			if (!_launcher.IsRunning(i.PID)) return false;
			if (now - i.CreationTime < StartupGrace && !i.HasCheckedIn) return true;
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
	}
}
