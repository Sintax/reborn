using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace ControlPanel.Core.ViewModels
{
	// The Users page: the add form, the list, the "Require keys to join" switch and each row's actions.
	// Keys are never put in Message; the only key on screen is PreviewKey while adding a user.
	public sealed class UsersViewModel : ObservableObject
	{
		private readonly AdminClient _client;
		private readonly PanelPaths _paths;
		private readonly IDialogs _dialogs;
		private readonly IProcessRunner _runner;
		private readonly TimeProvider _clock;

		public UsersViewModel(AdminClient client, PanelPaths paths, IDialogs dialogs, IProcessRunner runner, TimeProvider? clock = null)
		{
			_client = client;
			_paths = paths;
			_dialogs = dialogs;
			_runner = runner;
			_clock = clock ?? TimeProvider.System;
			GenerateKeyCommand = new RelayCommand(() => PreviewKey = Keys.New());
			SaveCommand = new AsyncRelayCommand(SaveAsync);
			ToggleRequireKeysCommand = new AsyncRelayCommand(ToggleRequireKeysAsync);
			OpenZipFolderCommand = new RelayCommand(OpenZipFolder);
		}

		public ObservableCollection<UserRow> Users { get; } = new();

		// Status text for this page: what the last action did, or why it didn't.
		private string _message = "";
		public string Message { get => _message; set => SetProperty(ref _message, value); }

		// --- add form ---

		private string _newName = "";
		public string NewName { get => _newName; set => SetProperty(ref _newName, value ?? ""); }

		private bool _newCanCreate;
		public bool NewCanCreate { get => _newCanCreate; set => SetProperty(ref _newCanCreate, value); }

		// The key Generate Key made; a new one on every press, sent on Save, then cleared.
		private string? _previewKey;
		public string? PreviewKey { get => _previewKey; set => SetProperty(ref _previewKey, value); }

		public IRelayCommand GenerateKeyCommand { get; }
		public IAsyncRelayCommand SaveCommand { get; }

		// --- the switch ---

		// Bind the check box one way and use ToggleRequireKeysCommand: turning keys off asks first.
		private bool _requireKeys = true;
		public bool RequireKeys { get => _requireKeys; private set => SetProperty(ref _requireKeys, value); }

		public IAsyncRelayCommand ToggleRequireKeysCommand { get; }

		// --- zips ---

		// The folder friend zips are written to, and the last one made.
		public string ZipFolder => _paths.ZipOutputDir;

		private string? _lastZipPath;
		public string? LastZipPath { get => _lastZipPath; private set => SetProperty(ref _lastZipPath, value); }

		public IRelayCommand OpenZipFolderCommand { get; }

		// --- loading ---

		public async Task ReloadAsync()
		{
			var users = await _client.GetUsersAsync();
			if (users.Ok) Update(users.Value!);
			else Message = users.Message;
		}

		// Rows are updated in place (matched by name) so the window keeps its selection.
		public void Update(UserListInfo info)
		{
			RequireKeys = info.RequireKeys;
			Dictionary<string, DateTime> zips = ZipRecords.Load(_paths.ZipRecordFile);
			DateTime now = _clock.GetUtcNow().UtcDateTime;
			var wanted = info.Users.Select(u => u.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
			for (int i = Users.Count - 1; i >= 0; i--)
				if (!wanted.Contains(Users[i].Name)) Users.RemoveAt(i);
			for (int i = 0; i < info.Users.Count; i++)
			{
				UserInfo u = info.Users[i];
				UserRow? row = Users.FirstOrDefault(r => string.Equals(r.Name, u.Name, StringComparison.OrdinalIgnoreCase));
				if (row == null)
				{
					row = new UserRow(this);
					Users.Insert(Math.Min(i, Users.Count), row);
				}
				else if (Users.IndexOf(row) != i) Users.Move(Users.IndexOf(row), i);
				row.Apply(u, zips.TryGetValue(u.Name, out DateTime zip) ? zip : null, now, _clock.LocalTimeZone);
			}
		}

		// --- actions ---

		private async Task SaveAsync()
		{
			string name = NewName.Trim();
			if (!Keys.ValidName(name))
			{
				Message = Keys.BadName;
				return;
			}
			if (PreviewKey == null)
			{
				Message = "Press Generate Key first.";
				return;
			}
			var added = await _client.AddUserAsync(name, PreviewKey, NewCanCreate);
			if (!added.Ok)
			{
				Message = added.Message;
				return;
			}
			NewName = "";
			NewCanCreate = false;
			PreviewKey = null;
			Message = $"{name} was added. Press Make zip on their row to make the zip to send them.";
			await ReloadAsync();
		}

		private async Task ToggleRequireKeysAsync()
		{
			bool turnOn = !RequireKeys;
			if (!turnOn && !_dialogs.Confirm("Let players join without a key? Then a player only needs a name, and anyone who finds this server can join."))
			{
				OnPropertyChanged(nameof(RequireKeys));   // puts the check box back
				return;
			}
			var set = await _client.SetRequireKeysAsync(turnOn);
			if (set.Ok)
			{
				RequireKeys = set.Value;
				Message = set.Value ? "Players now need their key to join." : "Players can now join without a key.";
			}
			else
			{
				Message = set.Message;
				OnPropertyChanged(nameof(RequireKeys));
			}
		}

		internal async Task ToggleCreateAsync(UserRow row)
		{
			bool allow = !row.CanCreateGames;
			var r = await _client.UpdateUserAsync(row.Name, canCreateGames: allow);
			if (!r.Ok)
			{
				Message = r.Message;
				row.Refresh();   // puts the check box back
				return;
			}
			Message = allow ? $"{row.Name} can now start games." : $"{row.Name} can no longer start games.";
			await ReloadAsync();
		}

		internal async Task RenameAsync(UserRow row)
		{
			string old = row.Name;
			string question = row.ConfirmName
				? $"Which name does the player with the key \"{old}\" play as? Type it exactly as they type it in the game."
				: $"New name for {old}:";
			string? typed = _dialogs.AskText(question, old)?.Trim();
			if (string.IsNullOrEmpty(typed) || (typed == old && !row.ConfirmName)) return;
			if (!Keys.ValidName(typed))
			{
				Message = Keys.BadName;
				return;
			}
			var r = await _client.UpdateUserAsync(old, newName: typed);
			if (!r.Ok)
			{
				Message = r.Message;
				return;
			}
			ZipRecords.Forget(_paths.ZipRecordFile, old);   // a zip made before carries the old name
			Message = row.ConfirmName
				? $"Saved. From now on this key only works with the name {typed}."
				: $"{old} is now {typed}. Their zip still has the old name, so make them a new zip.";
			await ReloadAsync();
		}

		internal async Task NewKeyAsync(UserRow row)
		{
			if (!_dialogs.Confirm($"Make a new key for {row.Name}? Their old key stops working at once, so they can't join until you send them a new zip."))
				return;
			var r = await _client.ReplaceKeyAsync(row.Name, Keys.New());
			if (!r.Ok)
			{
				Message = r.Message;
				return;
			}
			ZipRecords.Forget(_paths.ZipRecordFile, row.Name);
			Message = $"{row.Name} has a new key. Press Make zip to make their new zip, then send it to them.";
			await ReloadAsync();
		}

		internal async Task RevokeAsync(UserRow row)
		{
			if (!_dialogs.Confirm($"Remove {row.Name}? Their key stops working at once and they can no longer join."))
				return;
			var r = await _client.RemoveUserAsync(row.Name);
			if (!r.Ok)
			{
				Message = r.Message;
				return;
			}
			ZipRecords.Forget(_paths.ZipRecordFile, row.Name);
			Message = $"{row.Name} was removed.";
			await ReloadAsync();
		}

		internal async Task MakeZipAsync(UserRow row)
		{
			string name = row.Name;
			var key = await _client.GetKeyAsync(name);
			if (!key.Ok)
			{
				Message = key.Message;
				return;
			}
			DateTime now = _clock.GetUtcNow().UtcDateTime;
			// Off the window's thread: the installer zip can be large.
			var (ok, message, path) = await Task.Run(() =>
				FriendZip.Make(_paths.PlayerInstallerZip, name, key.Value!, _paths.ZipOutputDir, _paths.ZipRecordFile, now));
			Message = message;
			if (!ok) return;
			LastZipPath = path;
			row.SetZipMade(now, _clock.LocalTimeZone);
		}

		private void OpenZipFolder()
		{
			try { Directory.CreateDirectory(_paths.ZipOutputDir); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				Message = $"The folder {_paths.ZipOutputDir} could not be made.";
				return;
			}
			_runner.OpenFolder(_paths.ZipOutputDir);
		}
	}

	// One user in the list. Columns (spec): name, can create games, key made, zip made, last seen.
	public sealed class UserRow : ObservableObject
	{
		public UserRow(UsersViewModel owner)
		{
			ToggleCreateCommand = new AsyncRelayCommand(() => owner.ToggleCreateAsync(this));
			RenameCommand = new AsyncRelayCommand(() => owner.RenameAsync(this));
			NewKeyCommand = new AsyncRelayCommand(() => owner.NewKeyAsync(this));
			RevokeCommand = new AsyncRelayCommand(() => owner.RevokeAsync(this));
			MakeZipCommand = new AsyncRelayCommand(() => owner.MakeZipAsync(this));
		}

		private string _name = "";
		public string Name { get => _name; private set => SetProperty(ref _name, value); }

		// Bind the check box one way and use ToggleCreateCommand.
		private bool _canCreateGames;
		public bool CanCreateGames { get => _canCreateGames; private set => SetProperty(ref _canCreateGames, value); }

		// A key brought over from the old key files: the owner types the player's real name once.
		private bool _confirmName;
		public bool ConfirmName
		{
			get => _confirmName;
			private set
			{
				if (SetProperty(ref _confirmName, value))
				{
					OnPropertyChanged(nameof(NameNote));
					OnPropertyChanged(nameof(RenameLabel));
				}
			}
		}

		// "confirm name" next to the name of such a user, else "".
		public string NameNote => ConfirmName ? "confirm name" : "";

		// The rename button's text: confirming the name is the same action.
		public string RenameLabel => ConfirmName ? "Confirm name" : "Rename";

		// The date the user (and their key) was added; "" for keys brought over from the old files.
		private string _keyMadeText = "";
		public string KeyMadeText { get => _keyMadeText; private set => SetProperty(ref _keyMadeText, value); }

		// When their zip was last made by this panel; "" if never.
		private string _zipMadeText = "";
		public string ZipMadeText { get => _zipMadeText; private set => SetProperty(ref _zipMadeText, value); }

		// "never", "5 min ago", a date...
		private string _lastSeenText = "";
		public string LastSeenText { get => _lastSeenText; private set => SetProperty(ref _lastSeenText, value); }

		public IAsyncRelayCommand ToggleCreateCommand { get; }
		public IAsyncRelayCommand RenameCommand { get; }
		public IAsyncRelayCommand NewKeyCommand { get; }
		public IAsyncRelayCommand RevokeCommand { get; }
		public IAsyncRelayCommand MakeZipCommand { get; }

		internal void Apply(UserInfo u, DateTime? zipMadeUtc, DateTime nowUtc, TimeZoneInfo zone)
		{
			Name = u.Name;
			CanCreateGames = u.CanCreateGames;
			ConfirmName = u.ConfirmName;
			KeyMadeText = Display.Date(u.Added, zone);
			ZipMadeText = Display.Date(zipMadeUtc, zone);
			LastSeenText = Display.Ago(u.LastSeen, nowUtc, zone);
		}

		internal void SetZipMade(DateTime utc, TimeZoneInfo zone) => ZipMadeText = Display.Date(utc, zone);

		// Tells the window the check box's real value again after a refused change.
		internal void Refresh() => OnPropertyChanged(nameof(CanCreateGames));
	}
}
