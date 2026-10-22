namespace ControlPanel.Core
{
	// Starting and restarting matchmaking, as StartMatchmaking.bat and StopMatchmaking.bat do.
	// Never throws: anything that goes wrong comes back as a plain message.
	public sealed class MatchmakingControl
	{
		public const string ServiceProcess = "gamecontroller";
		public const string ServerProcess = "Serverborn";

		private readonly PanelPaths _paths;
		private readonly IProcessRunner _runner;
		private readonly Func<TimeSpan, Task> _delay;

		public MatchmakingControl(PanelPaths paths, IProcessRunner runner, Func<TimeSpan, Task>? delay = null)
		{
			_paths = paths;
			_runner = runner;
			_delay = delay ?? (t => Task.Delay(t));
		}

		// Starts matchmaking unless it is already running. The message says what happened.
		public async Task<(bool ok, string message)> StartAsync()
		{
			try
			{
				if (!File.Exists(_paths.MatchmakingExe))
					return (false, $"The matchmaking program is missing ({_paths.MatchmakingExe}). Install the server again.");
				if (_runner.IsRunning(ServiceProcess))
					return (false, "Matchmaking is already running but not answering yet. Wait a minute; if this stays, restart it on the Ports page.");

				// The game servers run as Serverborn.exe, a copy of Battleborn.exe (as StartMatchmaking.bat
				// makes it). The game is large, so copy off the window's thread.
				if (!File.Exists(_paths.ServerExe) && File.Exists(_paths.GameExe))
				{
					try { await Task.Run(() => File.Copy(_paths.GameExe, _paths.ServerExe)); }
					catch (Exception e) when (e is IOException or UnauthorizedAccessException)
					{
						return (false, $"Serverborn.exe could not be made in {_paths.Win64}. Copy Battleborn.exe to Serverborn.exe there, then try again.");
					}
				}

				if (!_runner.Start(_paths.MatchmakingExe, "", _paths.MatchmakingDir))
					return (false, "Matchmaking could not be started. Try StartMatchmaking.bat in the game folder.");
				return (true, "Starting matchmaking. This takes a few seconds; games appear in a minute or two.");
			}
			catch (Exception e)
			{
				return (false, "Could not start matchmaking: " + e.Message);
			}
		}

		// Stops matchmaking and every game server, runs whileStopped (if given), then starts it again.
		// Everyone in a game is disconnected.
		public async Task<(bool ok, string message)> RestartAsync(Func<Task>? whileStopped = null)
		{
			try
			{
				_runner.StopAll(ServiceProcess);
				_runner.StopAll(ServerProcess);
				for (int i = 0; i < 20 && _runner.IsRunning(ServiceProcess); i++) await _delay(TimeSpan.FromMilliseconds(500));
				if (_runner.IsRunning(ServiceProcess))
					return (false, "Matchmaking did not stop. Try again, or run StopMatchmaking.bat in the game folder.");
			}
			catch (Exception e)
			{
				return (false, "Could not stop matchmaking: " + e.Message);
			}
			if (whileStopped != null) await whileStopped();
			return await StartAsync();
		}
	}
}
