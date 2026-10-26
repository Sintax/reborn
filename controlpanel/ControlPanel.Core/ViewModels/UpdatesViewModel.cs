using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Reborn.Update;

namespace ControlPanel.Core.ViewModels
{
	// The Updates page: the installed version, a newer signed release (with its notes), Update now,
	// the saved versions to go back to, and what the last update or roll back did.
	// The swap itself is RebornUpdater.exe's job: the panel starts a copy of it and closes, and the
	// updater opens the panel again when it is done.
	public sealed class UpdatesViewModel : ObservableObject
	{
		public static readonly TimeSpan CheckEvery = TimeSpan.FromHours(6);

		public const string NotRecordedText = "not recorded";
		public const string NoUpdaterText = "The update has no updater (RebornUpdater.exe) in it, so it can't be installed. Nothing was changed.";
		public const string UpdaterMissingText = "RebornUpdater.exe is missing next to the Control Panel, so going back can't start. Nothing was changed.";
		public const string UpdaterWouldNotStartText = "The updater couldn't be started. Nothing was changed.";
		public const string HandingOverText = "The updater is taking over. The Control Panel closes now and opens again when it is done.";

		private readonly UpdateService _service;
		private readonly IDialogs _dialogs;
		private readonly IPanelApp _app;
		private readonly Func<int> _playersInGames;
		private readonly TimeProvider _clock;
		private UpdateManifest? _manifest;
		private DateTime? _nextCheckUtc;   // null: check on the first tick (the panel just opened)

		public UpdatesViewModel(UpdateService service, IDialogs dialogs, IPanelApp app, Func<int> playersInGames, TimeProvider? clock = null)
		{
			_service = service;
			_dialogs = dialogs;
			_app = app;
			_playersInGames = playersInGames;
			_clock = clock ?? TimeProvider.System;
			CheckCommand = new AsyncRelayCommand(CheckAsync, () => !Busy);
			UpdateNowCommand = new AsyncRelayCommand(UpdateNowAsync, () => !Busy && Available != null && Unfinished == null);
			RollBackCommand = new AsyncRelayCommand<string>(RollBackAsync, _ => !Busy);
			_service.DeleteOldUpdaterCopies();
			Reload();
		}

		// What reborn-deployed.txt says is installed ("1.0.3").
		private string _currentVersion = NotRecordedText;
		public string CurrentVersion { get => _currentVersion; private set => SetProperty(ref _currentVersion, value); }

		// The newer signed release ("1.0.4"), or null when there is none (or the last check failed).
		private string? _available;
		public string? Available
		{
			get => _available;
			private set
			{
				if (SetProperty(ref _available, value)) UpdateNowCommand.NotifyCanExecuteChanged();
			}
		}

		// That release's notes ("" without one).
		private string _notes = "";
		public string Notes { get => _notes; private set => SetProperty(ref _notes, value); }

		// What the last check or action found, in a sentence.
		private string _status = "";
		public string Status { get => _status; private set => SetProperty(ref _status, value); }

		// True while checking, downloading or handing over; the buttons are off then.
		private bool _busy;
		public bool Busy
		{
			get => _busy;
			private set
			{
				if (!SetProperty(ref _busy, value)) return;
				CheckCommand.NotifyCanExecuteChanged();
				UpdateNowCommand.NotifyCanExecuteChanged();
				RollBackCommand.NotifyCanExecuteChanged();
			}
		}

		// The saved versions to go back to, newest first (empty while an update is unfinished: then only
		// the roll back in UnfinishedText is allowed).
		public ObservableCollection<string> Kept { get; } = new();

		// "2026-10-26 14:02: Updated to 1.0.3. A game is up and running." (null before the first update).
		private string? _lastResult;
		public string? LastResult { get => _lastResult; private set => SetProperty(ref _lastResult, value); }

		private bool _lastResultOk;
		public bool LastResultOk { get => _lastResultOk; private set => SetProperty(ref _lastResultOk, value); }

		// The failed files and first error of the last run, for a closer look (null when there are none).
		private string? _lastResultDetails;
		public string? LastResultDetails { get => _lastResultDetails; private set => SetProperty(ref _lastResultDetails, value); }

		// The version to go back to after an update that broke off part way (null when nothing is unfinished).
		private string? _unfinished;
		public string? Unfinished
		{
			get => _unfinished;
			private set
			{
				if (SetProperty(ref _unfinished, value)) UpdateNowCommand.NotifyCanExecuteChanged();
			}
		}

		private string? _unfinishedText;
		public string? UnfinishedText { get => _unfinishedText; private set => SetProperty(ref _unfinishedText, value); }

		public IAsyncRelayCommand CheckCommand { get; }
		public IAsyncRelayCommand UpdateNowCommand { get; }
		public IAsyncRelayCommand<string> RollBackCommand { get; }

		// The check the last Tick started (for the tests to wait on).
		public Task? CheckTask { get; private set; }

		// From the panel's refresh timer: reads the files again and, at launch and every CheckEvery,
		// starts a check in the background. The check that started, or null.
		public Task? Tick()
		{
			Reload();
			DateTime now = _clock.GetUtcNow().UtcDateTime;
			if (Busy || (_nextCheckUtc != null && now < _nextCheckUtc.Value)) return null;
			CheckTask = CheckAsync();
			return CheckTask;
		}

		// Reads the installed version, the saved versions, the last result and any unfinished update.
		public void Reload()
		{
			CurrentVersion = _service.ReadDeployed() ?? NotRecordedText;

			UnfinishedUpdate? unfinished = _service.ReadUnfinished();
			Unfinished = unfinished?.From;
			UnfinishedText = unfinished == null ? null
				: (unfinished.To.Length > 0 ? $"The update to {unfinished.To} didn't finish" : "An update didn't finish")
					+ $", so the server may be a mix of two versions. Roll back to {unfinished.From} to put it right.";

			IReadOnlyList<string> kept = unfinished == null ? _service.Kept() : Array.Empty<string>();
			if (!kept.SequenceEqual(Kept))
			{
				Kept.Clear();
				foreach (string v in kept) Kept.Add(v);
			}

			LastUpdate? last = _service.ReadLastResult();
			LastResult = last == null ? null
				: last.When == null ? last.Message
				: $"{TimeZoneInfo.ConvertTime(last.When.Value, _clock.LocalTimeZone):yyyy-MM-dd HH:mm}: {last.Message}";
			LastResultOk = last?.Ok ?? false;
			LastResultDetails = string.IsNullOrWhiteSpace(last?.Details) ? null : last.Details;
		}

		private async Task CheckAsync()
		{
			if (Busy) return;
			Busy = true;
			Reload();
			Status = "Checking GitHub for a newer version...";
			try
			{
				UpdateCheck check = await _service.CheckAsync();
				_manifest = check.Manifest;
				Available = check.Manifest?.Version;
				Notes = check.Manifest?.Notes ?? "";
				Status = check.Message;
			}
			catch (Exception e)   // the service promises not to throw; just in case
			{
				_manifest = null;
				Available = null;
				Notes = "";
				Status = "The update check hit a problem: " + e.Message + " Nothing was changed.";
			}
			finally
			{
				_nextCheckUtc = _clock.GetUtcNow().UtcDateTime + CheckEvery;
				Busy = false;
			}
		}

		private async Task UpdateNowAsync()
		{
			if (Busy || _manifest == null || Unfinished != null) return;
			UpdateManifest manifest = _manifest;
			int players = _playersInGames();
			if (players > 0 && !_dialogs.Confirm(players == 1
				? "1 player is in a game. Updating stops that game. Update now?"
				: $"{players} players are in games. Updating stops those games. Update now?"))
				return;

			Busy = true;
			bool handedOver = false;
			try
			{
				Status = $"Downloading {manifest.Version} ({Megabytes(manifest.Size)}). This can take a few minutes.";
				StagedUpdate staged = await _service.DownloadAsync(manifest);
				if (!staged.Ok)
				{
					Status = staged.Message;
					return;
				}

				string? updater = _service.CopyUpdater(Path.Combine(staged.Win64Dir!, "ControlPanel", UpdateService.UpdaterExe));
				if (updater == null)
				{
					_service.ClearStaging();
					Status = NoUpdaterText;
					return;
				}
				string args = UpdateService.InstallArgs(staged.Win64Dir!, manifest.Version, _service.Win64, _app.ProcessId, _app.ExePath);
				handedOver = HandOver(updater, args);
				if (!handedOver) _service.ClearStaging();
			}
			finally
			{
				if (!handedOver) Busy = false;   // handed over: stay busy until the panel closes
			}
		}

		private Task RollBackAsync(string? version)
		{
			RollBack(version);
			return Task.CompletedTask;
		}

		private void RollBack(string? version)
		{
			version = (version ?? "").Trim();
			if (Busy || version.Length == 0) return;
			Reload();
			if (Unfinished != null ? version != Unfinished : !Kept.Contains(version))
			{
				Status = Unfinished != null
					? $"A previous update didn't finish. Roll back to {Unfinished} first."
					: $"There is no saved copy of {version} to go back to.";
				return;
			}
			if (!_dialogs.Confirm($"Go back to {version}? The server restarts.")) return;

			Busy = true;
			bool handedOver = false;
			try
			{
				// The installed updater, copied out so the roll back can replace it too.
				string? updater = _service.CopyUpdater(Path.Combine(Path.GetDirectoryName(_app.ExePath) ?? "", UpdateService.UpdaterExe));
				if (updater == null)
				{
					Status = UpdaterMissingText;
					return;
				}
				handedOver = HandOver(updater, UpdateService.RollbackArgs(version, _service.Win64, _app.ProcessId, _app.ExePath));
			}
			finally
			{
				if (!handedOver) Busy = false;
			}
		}

		// Starts the updater copy (in a window the owner can watch) and closes the panel, which the
		// updater waits for. False, with the copy deleted, if it would not start.
		private bool HandOver(string updater, string args)
		{
			bool started;
			try { started = _service.StartUpdater(updater, args); }
			catch (Exception) { started = false; }   // the runner promises not to throw; just in case
			if (!started)
			{
				try { File.Delete(updater); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
				Status = UpdaterWouldNotStartText;
				return false;
			}
			Status = HandingOverText;
			_app.Close();
			return true;
		}

		private static string Megabytes(long bytes) => $"{Math.Max(1, (bytes + 512 * 1024) / (1024 * 1024))} MB";
	}
}
