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

		// Bind the check box one way and use ToggleRequireKeysCommand: turning keys off or on asks first.
		private bool _requireKeys = true;
		public bool RequireKeys { get => _requireKeys; private set => SetProperty(ref _requireKeys, value); }

		public IAsyncRelayCommand ToggleRequireKeysCommand { get; }

		public const string KeysOnQuestion = "Require keys to join? Players without a key will be dropped from games. "
			+ "From then on, only players on the list can join, each with the key in their zip.";

		public const string NoListText = "There is no user list yet, so anyone can join. The first player you add starts the list. "
			+ "From then on, only players on the list can join.";

		// False when the server has no user list file: then anyone can join, and the switch is off and
		// can't be used until the first player is added. (An older matchmaking does not say: true.)
		private bool _hasUserList = true;
		public bool HasUserList
		{
			get => _hasUserList;
			private set
			{
				if (SetProperty(ref _hasUserList, value)) OnPropertyChanged(nameof(NoListNote));
			}
		}

		// Shown on the page while there is no user list ("" once there is one).
		public string NoListNote => HasUserList ? "" : NoListText;

		// --- zips ---

		// The folder friend zips are written to, and the last one made.
		public string ZipFolder => _paths.ZipOutputDir;

		private string? _lastZipPath;
		public string? LastZipPath { get => _lastZipPath; private set => SetProperty(ref _lastZipPath, value); }

		public IRelayCommand OpenZipFolderCommand { get; }

		// The folder with 7z.exe and 7z.sfx; null looks for 7-Zip (tests point it at a fake one).
		public string? SevenZipDir { get; set; }

		// The server's version (from matchmaking), put on top of the guide in each friend zip.
		internal string ServerVersion { get; set; } = "";

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
			HasUserList = info.UserListExists ?? true;
			RequireKeys = HasUserList && info.RequireKeys;
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

			var seen = (info.SeenNotListed ?? Array.Empty<SeenName>())
				.Select(s => new SeenRow(s.Name, Display.Ago(s.LastSeen, now, _clock.LocalTimeZone)))
				.ToList();
			if (!seen.SequenceEqual(SeenNotListed))
			{
				SeenNotListed.Clear();
				foreach (SeenRow s in seen) SeenNotListed.Add(s);
			}
			OnPropertyChanged(nameof(SeenNotListedEmptyText));
		}

		// --- seen recently but not on the list ---

		// Names that joined a game lately but match nobody on the list, newest first. The owner picks
		// the real name of a "confirm name" key from here. Names only, never keys.
		public ObservableCollection<SeenRow> SeenNotListed { get; } = new();

		public const string NobodySeenText = "Nobody yet.";

		// Shown in place of the list when it is empty ("" when it has names).
		public string SeenNotListedEmptyText => SeenNotListed.Count == 0 ? NobodySeenText : "";

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
			if (!HasUserList && !_dialogs.Confirm(FirstAddQuestion(name, NewCanCreate))) return;
			var added = await _client.AddUserAsync(name, PreviewKey, NewCanCreate);
			if (!added.Ok)
			{
				Message = added.Message;
				return;
			}
			NewName = "";
			NewCanCreate = false;
			PreviewKey = null;
			Message = $"{name} was added. Press Make file on their row to make the file to send them.";
			await ReloadAsync();
		}

		// Adding the first player starts the user list: from then on, everyone else is shut out.
		internal static string FirstAddQuestion(string name, bool canCreateGames) =>
			$"Start the user list with {name}? From now on, only players on the list can join, each with the key in their zip. "
			+ "Anyone else in a game now will be dropped."
			+ (canCreateGames ? "" : $"\n\n{name} can't start games, so nobody on the list could start one. "
				+ "Add yourself first, with Can create games ticked.");

		private async Task ToggleRequireKeysAsync()
		{
			if (!HasUserList)
			{
				Message = NoListText;
				OnPropertyChanged(nameof(RequireKeys));   // puts the check box back
				return;
			}
			bool turnOn = !RequireKeys;
			string question = turnOn
				? KeysOnQuestion
				: "Let players join without a key? Then a player only needs a name, and anyone who finds this server can join.";
			if (!_dialogs.Confirm(question))
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
			string question = row.ConfirmName ? ConfirmNameQuestion(old) : RenameQuestion(old);
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
				? $"Saved. From now on this key only works with the name {typed}. You can make their zip now."
				: $"{old} is now {typed}. Press Make file and send them the new file: they can't join until they install it.";
			await ReloadAsync();
		}

		// What the rename box says before anything is saved (the change takes effect at once).
		internal static string RenameQuestion(string old) =>
			$"New name for {old}:\n\n"
			+ $"This takes effect at once. {old} then needs a new zip with the new name, and can't join until they install it. "
			+ "If they're in a game now, they'll be dropped.";

		// A key brought over from the old key files: the label is not their in-game name. Typing their
		// real name changes nothing for them; a wrong one locks them out, so say that, and only as an "if".
		internal static string ConfirmNameQuestion(string label) =>
			$"Which name does the player with the key \"{label}\" play as? Type it exactly as it appears in the game, "
			+ "with the same capital letters.\n\n"
			+ "From then on their key only works with that name. If it doesn't match, they'll be dropped from any game "
			+ "they're in and can't join until you fix it.";

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
			Message = $"{row.Name} has a new key. Press Make file to make their new file, then send it to them.";
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
			if (row.ConfirmName)
			{
				// The stored name is only a label: a zip would make it their in-game name and Steam identity.
				Message = $"{UserRow.ConfirmFirstNote}: press Confirm name on {name}'s row, so the zip has the name they play as.";
				return;
			}
			var key = await _client.GetKeyAsync(name);
			if (!key.Ok)
			{
				Message = key.Message;
				return;
			}
			DateTime now = _clock.GetUtcNow().UtcDateTime;
			// Off the window's thread: the installer zip can be large.
			var (ok, message, path) = await Task.Run(() =>
				FriendZip.Make(_paths.PlayerInstallerZip, name, key.Value!, _paths.ZipOutputDir, _runner, _paths.ZipRecordFile, now, ServerVersion, SevenZipDir));
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

	// One name seen in a game lately that is not on the list: the name and "5 min ago".
	public sealed record SeenRow(string Name, string LastSeenText);

	// One user in the list. Columns (spec): name, can create games, key made, zip made, last seen.
	public sealed class UserRow : ObservableObject
	{
		public UserRow(UsersViewModel owner)
		{
			ToggleCreateCommand = new AsyncRelayCommand(() => owner.ToggleCreateAsync(this));
			RenameCommand = new AsyncRelayCommand(() => owner.RenameAsync(this));
			NewKeyCommand = new AsyncRelayCommand(() => owner.NewKeyAsync(this));
			RevokeCommand = new AsyncRelayCommand(() => owner.RevokeAsync(this));
			MakeZipCommand = new AsyncRelayCommand(() => owner.MakeZipAsync(this), () => !ConfirmName);
		}

		public const string ConfirmFirstNote = "Confirm their name first";

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
					OnPropertyChanged(nameof(MakeZipNote));
					MakeZipCommand.NotifyCanExecuteChanged();
				}
			}
		}

		// Why Make file is off on such a row ("" when it is on): until the owner confirms the name, the
		// stored name is only a label, and a zip would make it the player's in-game name.
		public string MakeZipNote => ConfirmName ? ConfirmFirstNote : "";

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
