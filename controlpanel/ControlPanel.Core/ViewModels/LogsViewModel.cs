using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace ControlPanel.Core.ViewModels
{
	// The Logs page: the end of matchmaking.log, and a button for the game servers' log folders.
	public sealed class LogsViewModel : ObservableObject
	{
		public const int LineCount = 200;
		private const int TailBytes = 1024 * 1024;   // enough for 200 lines of any real log

		private readonly PanelPaths _paths;
		private readonly IProcessRunner _runner;

		public LogsViewModel(PanelPaths paths, IProcessRunner runner)
		{
			_paths = paths;
			_runner = runner;
			RefreshCommand = new RelayCommand(Refresh);
			OpenGameLogsCommand = new RelayCommand(OpenGameLogs);
		}

		// The last 200 lines, oldest first.
		private string _text = "";
		public string Text { get => _text; private set => SetProperty(ref _text, value); }

		private string _message = "";
		public string Message { get => _message; private set => SetProperty(ref _message, value); }

		public string GameLogsFolder => _paths.GameLogsDir;

		public IRelayCommand RefreshCommand { get; }
		public IRelayCommand OpenGameLogsCommand { get; }

		private void Refresh()
		{
			if (!File.Exists(_paths.MatchmakingLog))
			{
				Text = "";
				Message = "There is no matchmaking log yet. It appears once matchmaking has started.";
				return;
			}
			try
			{
				// Matchmaking keeps the file open for writing, so share it.
				using var stream = new FileStream(_paths.MatchmakingLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
				bool cut = stream.Length > TailBytes;
				if (cut) stream.Seek(-TailBytes, SeekOrigin.End);
				using var reader = new StreamReader(stream, Encoding.UTF8);
				var lines = reader.ReadToEnd().Split('\n').Select(l => l.TrimEnd('\r')).ToList();
				if (cut && lines.Count > 0) lines.RemoveAt(0);                          // a part line
				if (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);
				Text = string.Join(Environment.NewLine, lines.Skip(Math.Max(0, lines.Count - LineCount)));
				Message = $"The last {Math.Min(lines.Count, LineCount)} lines of matchmaking.log, read at {DateTime.Now:HH:mm:ss}.";
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				Message = "The matchmaking log could not be read just now. Press Refresh to try again.";
			}
		}

		private void OpenGameLogs()
		{
			try { Directory.CreateDirectory(_paths.GameLogsDir); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				Message = $"The folder {_paths.GameLogsDir} could not be made.";
				return;
			}
			_runner.OpenFolder(_paths.GameLogsDir);
		}
	}
}
