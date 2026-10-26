using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace ControlPanel.Core.ViewModels
{
	// The whole panel: the header (address, version, matchmaking up or not) and the pages.
	// The window calls RefreshAsync every RefreshInterval on its own thread; nothing here throws when
	// matchmaking is down, it shows Banner instead and recovers on the next refresh.
	public sealed class MainViewModel : ObservableObject
	{
		public static readonly TimeSpan RefreshInterval = TimeSpan.FromSeconds(5);
		public const string StartingText = "Matchmaking is starting. This takes a few seconds.";

		private readonly AdminClient _client;
		private readonly IClipboard _clipboard;
		private readonly MatchmakingControl _control;
		private readonly TimeProvider _clock;
		private readonly int _port;
		private string? _publicIp;
		private DateTime? _startAskedUtc;
		private bool _refreshing;

		// updates: where server updates come from (GitHub with the release key unless a test gives its own).
		public MainViewModel(AdminClient client, PanelPaths paths, IClipboard clipboard, IProcessRunner runner, IDialogs dialogs,
			ISystemMemory memory, IPanelApp app, TimeProvider? clock = null, UpdateService? updates = null)
		{
			_client = client;
			_clipboard = clipboard;
			_clock = clock ?? TimeProvider.System;
			_port = paths.CoordinatorPort;
			_control = new MatchmakingControl(paths, runner);
			Games = new GamesViewModel();
			Users = new UsersViewModel(client, paths, dialogs, runner, _clock);
			Ports = new PortsViewModel(client, paths, runner, dialogs, memory, _control);
			Logs = new LogsViewModel(paths, runner);
			Updates = new UpdatesViewModel(updates ?? new UpdateService(paths, runner), dialogs, app, () => Games.PlayersInGames, _clock);
			Updates.PropertyChanged += (_, e) =>
			{
				if (e.PropertyName is nameof(UpdatesViewModel.Available) or nameof(UpdatesViewModel.Unfinished))
					OnPropertyChanged(nameof(UpdateLine));
			};
			CopyAddressCommand = new RelayCommand(CopyAddress, () => _publicIp != null);
			StartMatchmakingCommand = new AsyncRelayCommand(StartMatchmakingAsync, () => !MatchmakingUp);
		}

		public GamesViewModel Games { get; }
		public UsersViewModel Users { get; }
		public PortsViewModel Ports { get; }
		public LogsViewModel Logs { get; }
		public UpdatesViewModel Updates { get; }

		// "Update available: 1.0.4" in the header while there is one ("" otherwise, and while an update
		// is unfinished: then the Updates page's roll back comes first).
		public string UpdateLine => Updates.Available == null || Updates.Unfinished != null ? "" : "Update available: " + Updates.Available;

		// "203.0.113.10:5000": what players' games connect to. Shown only inside the panel.
		private string _serverAddress = "";
		public string ServerAddress { get => _serverAddress; private set => SetProperty(ref _serverAddress, value); }

		private string _version = "";
		public string Version { get => _version; private set => SetProperty(ref _version, value); }

		private bool _matchmakingUp;
		public bool MatchmakingUp
		{
			get => _matchmakingUp;
			private set
			{
				if (SetProperty(ref _matchmakingUp, value)) StartMatchmakingCommand.NotifyCanExecuteChanged();
			}
		}

		// A plain sentence across the top when something is wrong (null when all is well).
		// Show the Start button with it while MatchmakingUp is false.
		private string? _banner;
		public string? Banner { get => _banner; private set => SetProperty(ref _banner, value); }

		// "2 games running. Matchmaking started 10:22."
		private string _statusLine = "";
		public string StatusLine { get => _statusLine; private set => SetProperty(ref _statusLine, value); }

		public IRelayCommand CopyAddressCommand { get; }
		public IAsyncRelayCommand StartMatchmakingCommand { get; }

		public async Task RefreshAsync()
		{
			if (_refreshing) return;   // the last one is still waiting for matchmaking
			_refreshing = true;
			try
			{
				DateTime now = _clock.GetUtcNow().UtcDateTime;
				ExpireCopyNotice(now);
				// The Updates page reads its files, and checks GitHub at launch and every 6 hours. The check
				// runs on its own (gh is slow), so this refresh never waits for it.
				_ = Updates.Tick();
				var status = await _client.GetStatusAsync();
				if (!status.Ok)
				{
					MatchmakingUp = !status.NotRunning;
					bool starting = _startAskedUtc != null && now - _startAskedUtc.Value < TimeSpan.FromSeconds(30);
					Banner = status.NotRunning && starting ? StartingText : status.Message;
					StatusLine = "";
					if (status.NotRunning)
					{
						// Whatever was shown is no longer known to be true.
						Games.Clear();
						Ports.SetGames(Array.Empty<GameInfo>());
					}
					return;
				}

				ServerStatus s = status.Value!;
				MatchmakingUp = true;
				_startAskedUtc = null;
				Banner = null;
				Version = s.Version == "" ? "unknown" : s.Version;
				Users.ServerVersion = s.Version;
				_publicIp = string.IsNullOrEmpty(s.PublicIp) ? null : s.PublicIp;
				ServerAddress = _publicIp == null ? "Finding this server's address..." : $"{_publicIp}:{_port}";
				CopyAddressCommand.NotifyCanExecuteChanged();
				string running = s.GamesRunning == 1 ? "1 game" : s.GamesRunning + " games";
				StatusLine = s.MatchmakingStartedUtc == default   // an older matchmaking that does not say
					? $"{running} running."
					: $"{running} running. Matchmaking started {TimeZoneInfo.ConvertTimeFromUtc(Display.Utc(s.MatchmakingStartedUtc), _clock.LocalTimeZone):yyyy-MM-dd HH:mm}.";
				Ports.Update(s);

				var games = await _client.GetGamesAsync();
				if (games.Ok)
				{
					Games.Update(games.Value!, now);
					Ports.SetGames(games.Value!);
				}
				else Banner = games.Message;

				var users = await _client.GetUsersAsync();
				if (users.Ok) Users.Update(users.Value!);
				else Users.Message = users.Message;   // e.g. the user file is damaged: the Users page says so
			}
			finally
			{
				_refreshing = false;
			}
		}

		// A short note about the Copy button ("could not be copied"). Kept apart from Banner, which every
		// refresh rewrites; it clears on the next Copy or after CopyNoticeFor.
		public static readonly TimeSpan CopyNoticeFor = TimeSpan.FromSeconds(15);
		public const string CopyBusyText = "The address could not be copied (another program is using the clipboard). Try again.";
		private DateTime _copyNoticeUtc;

		private string? _copyNotice;
		public string? CopyNotice { get => _copyNotice; private set => SetProperty(ref _copyNotice, value); }

		private void CopyAddress()
		{
			if (_publicIp == null) return;
			CopyNotice = null;
			try { _clipboard.SetText($"{_publicIp}:{_port}"); }
			catch (Exception e) when (e is System.Runtime.InteropServices.ExternalException or InvalidOperationException)
			{
				CopyNotice = CopyBusyText;
				_copyNoticeUtc = _clock.GetUtcNow().UtcDateTime;
			}
		}

		private void ExpireCopyNotice(DateTime nowUtc)
		{
			if (CopyNotice != null && nowUtc - _copyNoticeUtc >= CopyNoticeFor) CopyNotice = null;
		}

		private async Task StartMatchmakingAsync()
		{
			var (ok, message) = await _control.StartAsync();
			if (ok) _startAskedUtc = _clock.GetUtcNow().UtcDateTime;
			Banner = message;
		}
	}
}
