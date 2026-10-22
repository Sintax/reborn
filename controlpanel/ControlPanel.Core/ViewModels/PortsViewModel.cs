using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace ControlPanel.Core.ViewModels
{
	// The Ports page: one UDP port per game that can run at once. Saving stores the list for
	// matchmaking's next start (nothing running is stopped) and updates the Windows Firewall rule.
	public sealed class PortsViewModel : ObservableObject
	{
		public const int MaxPorts = 16;
		public const string ReminderText = "Also open these UDP ports in your hosting company's firewall.";
		public const string RestartNeeded = "Restart matchmaking to use the new ports.";

		private readonly AdminClient _client;
		private readonly PanelPaths _paths;
		private readonly IProcessRunner _runner;
		private readonly IDialogs _dialogs;
		private readonly ISystemMemory _memory;
		private readonly MatchmakingControl _control;

		private List<int> _saved = new();                 // what appsettings.json holds (the next start)
		private List<int> _inUse = new();                 // what the running matchmaking started with
		private IReadOnlyList<GameInfo> _games = Array.Empty<GameInfo>();

		public PortsViewModel(AdminClient client, PanelPaths paths, IProcessRunner runner, IDialogs dialogs, ISystemMemory memory,
			MatchmakingControl control)
		{
			_client = client;
			_paths = paths;
			_runner = runner;
			_dialogs = dialogs;
			_memory = memory;
			_control = control;
			AddPortCommand = new RelayCommand(AddPort);
			RemovePortCommand = new RelayCommand<int>(RemovePort);
			SaveCommand = new AsyncRelayCommand(SaveAsync);
			RestartCommand = new AsyncRelayCommand(RestartAsync);
			Ports.CollectionChanged += (_, _) => Recompute();
		}

		// The list being edited (the saved list until the owner changes it).
		public ObservableCollection<int> Ports { get; } = new();

		// The text box next to Add.
		private string _newPort = "";
		public string NewPort { get => _newPort; set => SetProperty(ref _newPort, value ?? ""); }

		public IRelayCommand AddPortCommand { get; }
		public IRelayCommand<int> RemovePortCommand { get; }
		public IAsyncRelayCommand SaveCommand { get; }
		public IAsyncRelayCommand RestartCommand { get; }

		// True while the list differs from the saved one (Save to keep it).
		public bool HasChanges => !SameSet(Ports, _saved);

		public string Reminder => ReminderText;

		// "Restart matchmaking to use the new ports." while the saved ports are not the ones in use.
		private string? _restartNote;
		public string? RestartNote { get => _restartNote; private set => SetProperty(ref _restartNote, value); }

		// Set while a port that a game is running on is not in the list.
		private string? _warning;
		public string? Warning { get => _warning; private set => SetProperty(ref _warning, value); }

		// What the last action did, or why not.
		private string _message = "";
		public string Message { get => _message; private set => SetProperty(ref _message, value); }

		// Memory needed for this many games against this machine's memory; null if it can't be read.
		private MemoryPlan? _memoryPlan;
		public MemoryPlan? MemoryPlan { get => _memoryPlan; private set => SetProperty(ref _memoryPlan, value); }

		public string MemoryText => MemoryPlan?.Summary ?? "This server's memory could not be read.";
		public string MemoryWarning => MemoryPlan?.Warning ?? "";

		// --- from the refresh ---

		public void Update(ServerStatus status)
		{
			bool editing = HasChanges;
			_saved = status.GamePorts.ToList();
			_inUse = status.GamePortsInUse.ToList();
			if (!editing && !Ports.SequenceEqual(_saved))
			{
				Ports.Clear();
				foreach (int p in _saved) Ports.Add(p);
			}
			RestartNote = SameSet(_saved, _inUse) ? null : RestartNeeded;
			Recompute();
		}

		public void SetGames(IReadOnlyList<GameInfo> games)
		{
			_games = games;
			Recompute();
		}

		private void Recompute()
		{
			OnPropertyChanged(nameof(HasChanges));
			MemoryReading? reading = _memory.Read();
			MemoryPlan = reading == null ? null : new MemoryPlan(reading.TotalGb, reading.FreeGb, Ports.Count);
			OnPropertyChanged(nameof(MemoryText));
			OnPropertyChanged(nameof(MemoryWarning));
			var busy = _games.Select(g => g.Port).Where(p => !Ports.Contains(p)).Distinct().OrderBy(p => p).ToList();
			Warning = busy.Count == 0 ? null
				: $"A game is running on port {string.Join(", ", busy)}. It keeps running; the port is dropped the next time matchmaking restarts.";
		}

		// --- editing ---

		private void AddPort()
		{
			string text = NewPort.Trim();
			if (!int.TryParse(text, out int port))
			{
				Message = "Type a port number, for example 7780.";
				return;
			}
			if (port < 1024 || port > 65535)
			{
				Message = "Game ports must be numbers from 1024 to 65535.";
				return;
			}
			if (Ports.Contains(port))
			{
				Message = $"Port {port} is already in the list.";
				return;
			}
			if (Ports.Count >= MaxPorts)
			{
				Message = $"You can have at most {MaxPorts} game ports.";
				return;
			}
			int at = 0;
			while (at < Ports.Count && Ports[at] < port) at++;
			Ports.Insert(at, port);
			NewPort = "";
			Message = "Press Save to keep the change.";
		}

		private void RemovePort(int port)
		{
			if (!Ports.Contains(port)) return;
			if (Ports.Count == 1)
			{
				Message = "Keep at least one game port.";
				return;
			}
			Ports.Remove(port);
			Message = "Press Save to keep the change.";
		}

		// --- saving and restarting ---

		private async Task SaveAsync()
		{
			List<int> list = Ports.ToList();
			var saved = await _client.SetPortsAsync(list);
			if (!saved.Ok)
			{
				Message = saved.Message;
				return;
			}
			_saved = list;
			RestartNote = SameSet(_saved, _inUse) ? null : RestartNeeded;
			Recompute();

			// Ports still in use stay open until the restart, so a running game is not cut off.
			List<int> open = list.Union(_inUse).OrderBy(p => p).ToList();
			bool firewall = await ApplyFirewallAsync(open);
			string restart = RestartNote == null ? "" : " " + RestartNeeded;
			Message = firewall
				? $"Saved. Windows Firewall lets players in on UDP {string.Join(", ", open)}.{restart}"
				: $"Saved, but Windows Firewall was not changed (the administrator question was refused or failed). Press Save again to retry.{restart}";
		}

		// Two runs as administrator: remove the old rule (fails harmlessly if there is none), add the new one.
		private async Task<bool> ApplyFirewallAsync(IEnumerable<int> ports)
		{
			string[] args = Firewall.ArgsFor(ports, _paths.ServerExe);
			if (await _runner.RunElevated("netsh", args[0]) == -1) return false;   // the owner said no
			return await _runner.RunElevated("netsh", args[1]) == 0;
		}

		private async Task RestartAsync()
		{
			int players = _games.Sum(g => g.Players?.Count ?? 0);
			string question = players > 0
				? $"{players} {(players == 1 ? "player is" : "players are")} in a game right now. Restarting matchmaking stops every game and disconnects them. Restart now?"
				: _games.Count > 0
					? "Restarting matchmaking stops the games that are running (nobody is in them). Restart now?"
					: "Restart matchmaking now? Games come back in a minute or two.";
			if (!_dialogs.Confirm(question)) return;

			bool narrowed = true;
			bool narrow = _inUse.Except(_saved).Any();   // ports no longer used can be closed now
			var (ok, message) = await _control.RestartAsync(narrow ? async () => narrowed = await ApplyFirewallAsync(_saved) : null);
			Message = narrowed ? message : message + " Windows Firewall still lets players in on the removed ports; press Save to retry.";
			if (ok) _games = Array.Empty<GameInfo>();
		}

		private static bool SameSet(IEnumerable<int> a, IEnumerable<int> b) => a.ToHashSet().SetEquals(b);
	}
}
