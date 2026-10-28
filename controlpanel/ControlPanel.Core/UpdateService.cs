using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.RegularExpressions;
using Reborn.Update;

namespace ControlPanel.Core
{
	public enum CheckOutcome { UpToDate, Available, Problem }

	// What a check found. Manifest is set only for Available (signed, readable and newer).
	public sealed record UpdateCheck(CheckOutcome Outcome, UpdateManifest? Manifest, string Message);

	// A downloaded, checked and unpacked update. Win64Dir is the zip's Win64 folder (what the updater installs).
	public sealed record StagedUpdate(bool Ok, string? Win64Dir, string Message);

	// RebornVersions\last-update.json, written by the Installer after every update or roll back.
	public sealed record LastUpdate(string Version, bool Ok, string Message, string Details, DateTimeOffset? When);

	// RebornVersions\in-progress.json: an update or roll back that broke off part way. CanRollBack is false
	// when the roll back that would put it right can't run (the mark can't be read, or the saved copy of
	// From is gone); MarkPath is the mark's full path, the file the owner then deletes by hand.
	public sealed record UnfinishedUpdate(string From, string To, bool CanRollBack = true, string MarkPath = "");

	// Server updates from the private builds repo on GitHub, through the gh program (signed in on the VM),
// or, on a server without gh, straight from GitHub's web interface with the kept read-only token.
	// Nothing here changes the installed server: the swap is RebornUpdater.exe's job.
	public sealed partial class UpdateService
	{
		public const string Repo = "Sintax/reborn-builds";
		public const string ManifestFile = "update.json";
		public const string SignatureFile = "update.json.sig";
		public const string StagingFolder = "staging";
		public const string UpdaterExe = "RebornUpdater.exe";

		public const string NoGitHubText = "This server can't reach the builds on GitHub right now. Nothing was changed.";
		public const string NoSignedUpdateText = "The newest build on GitHub has no signed server update. Nothing was changed.";
		public const string BadSignatureText = "The update on GitHub isn't signed with this server's key, so it was not used. Nothing was changed.";
		public const string UnreadableText = "The update on GitHub couldn't be read, so it was not used. Nothing was changed.";
		public const string UpToDateText = "This server is up to date.";
		public const string ReadyText = "It's signed and ready. Update now installs it and restarts the server.";
		public const string MismatchText = "The downloaded file isn't the one the signed update describes, so it was not used. Nothing was changed.";
		public const string NoServerFilesText = "The downloaded update has no server files in it, so it was not used. Nothing was changed.";

		private readonly PanelPaths _paths;
		private readonly IProcessRunner _runner;
		private readonly string _publicKey;
		private readonly string _gh;
		private readonly HttpClient _http;
		private readonly Func<string?> _token;

		// Redirects are followed by hand: the token goes to GitHub, never on to the file host it points at.
		private static readonly Lazy<HttpClient> DefaultHttp = new(() =>
			new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromMinutes(30) });

		// publicKey: the release signing key (UpdateKey.PublicKey unless a test gives its own).
		// tempDir: where the check downloads to and the updater is copied to (%TEMP% unless given).
		public UpdateService(PanelPaths paths, IProcessRunner runner, string? publicKey = null, string? tempDir = null, string gh = "gh", HttpClient? http = null, Func<string?>? token = null)
		{
			_paths = paths;
			_runner = runner;
			_publicKey = publicKey ?? UpdateKey.PublicKey;
			_gh = gh;
			_http = http ?? DefaultHttp.Value;
			_token = token ?? (() => GitHubToken.Load());
			TempDir = Path.GetFullPath(tempDir ?? Path.GetTempPath()).TrimEnd('\\', '/');
		}

		public string Win64 => _paths.Win64;
		public string TempDir { get; }
		public string VersionsDir => Path.Combine(_paths.Win64, Installer.VersionsFolder);
		public string StagingDir => Path.Combine(VersionsDir, StagingFolder);

		// reborn-deployed.txt, trimmed: "1.0.3", or an older "2026-10-22 bab2c4e". Null when there is none.
		public string? ReadDeployed()
		{
			try
			{
				string text = File.ReadAllText(Path.Combine(_paths.Win64, Installer.DeployedFile)).Trim();
				return text.Length == 0 ? null : text;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
		}

		// Downloads the newest release's update.json and its signature, checks the signature over the
		// exact bytes, and compares the build with the installed one. Never throws.
		public async Task<UpdateCheck> CheckAsync()
		{
			string dir = Path.Combine(TempDir, "reborn-update-check-" + Guid.NewGuid().ToString("N"));
			try
			{
				Directory.CreateDirectory(dir);
				string args = $"release download --repo {Repo} --pattern {ManifestFile} --pattern {SignatureFile} --dir \"{dir}\" --clobber";
				var (exit, output) = await RunAsync(args, null, new[] { ManifestFile, SignatureFile }, dir);
				if (exit != 0) return Problem(NothingToGet(output) ? NoSignedUpdateText : NoGitHubText);

				string manifestPath = Path.Combine(dir, ManifestFile), sigPath = Path.Combine(dir, SignatureFile);
				if (!File.Exists(manifestPath) || !File.Exists(sigPath)) return Problem(NoSignedUpdateText);
				byte[] bytes = await File.ReadAllBytesAsync(manifestPath);
				string sig = (await File.ReadAllTextAsync(sigPath)).Trim();
				if (!Signature.Verify(bytes, sig, _publicKey)) return Problem(BadSignatureText);

				UpdateManifest? manifest = UpdateManifest.Parse(new UTF8Encoding(false).GetString(bytes));
				if (manifest == null) return Problem(UnreadableText);

				int current = Versions.Parse(ReadDeployed() ?? "");
				return Versions.Newer(manifest.Build, current)
					? new UpdateCheck(CheckOutcome.Available, manifest, ReadyText)
					: new UpdateCheck(CheckOutcome.UpToDate, null, UpToDateText);
			}
			catch (Exception e)
			{
				return Problem("The update check hit a problem: " + e.Message + " Nothing was changed.");
			}
			finally
			{
				TryDeleteDirectory(dir);
			}
		}

		// Downloads the release's zip into RebornVersions\staging\, checks its size and SHA-256 against the
		// signed manifest, and unpacks it into staging\<version>\. On any problem staging is deleted.
		public async Task<StagedUpdate> DownloadAsync(UpdateManifest manifest)
		{
			string staging = StagingDir;
			try
			{
				TryDeleteDirectory(staging);   // whatever an earlier attempt left
				Directory.CreateDirectory(staging);
				string args = $"release download {manifest.Version} --repo {Repo} --pattern \"{manifest.Zip}\" --dir \"{staging}\" --clobber";
				var (exit, _) = await RunAsync(args, manifest.Version, new[] { manifest.Zip }, staging);
				if (exit != 0) return Failed(staging, NoGitHubText);

				string zip = Path.Combine(staging, manifest.Zip);
				if (!File.Exists(zip) || new FileInfo(zip).Length != manifest.Size) return Failed(staging, MismatchText);
				string hash = await Task.Run(() =>
				{
					using FileStream s = File.OpenRead(zip);
					return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
				});
				if (hash != manifest.Sha256) return Failed(staging, MismatchText);

				string target = Path.Combine(staging, manifest.Version);
				await Task.Run(() => ZipFile.ExtractToDirectory(zip, target));
				File.Delete(zip);
				string win64 = Path.Combine(target, "Win64");
				if (!Directory.Exists(win64) || !Directory.EnumerateFiles(win64, "*", SearchOption.AllDirectories).Any())
					return Failed(staging, NoServerFilesText);
				return new StagedUpdate(true, win64, $"{manifest.Version} is downloaded and checked.");
			}
			catch (Exception e)
			{
				return Failed(staging, "The download hit a problem: " + e.Message + " Nothing was changed.");
			}
		}

		// Deletes RebornVersions\staging (after a download that won't be installed).
		public void ClearStaging() => TryDeleteDirectory(StagingDir);

		// Copies an updater exe to %TEMP%\RebornUpdater-<guid>.exe, so the copy that runs can replace the
		// installed one. The copy's path, or null if it couldn't be made.
		public string? CopyUpdater(string source)
		{
			try
			{
				if (!File.Exists(source)) return null;
				Directory.CreateDirectory(TempDir);
				string target = Path.Combine(TempDir, $"RebornUpdater-{Guid.NewGuid():N}.exe");
				File.Copy(source, target);
				return target;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
		}

		// Starts the updater copy in a window the owner can watch, from its own folder.
		public bool StartUpdater(string exe, string args) =>
			_runner.Start(exe, args, Path.GetDirectoryName(exe) ?? TempDir, minimized: false);

		// Deletes the updater copies earlier updates left in %TEMP% (each is about 70 MB). One that is still
		// running can't be deleted and is left for next time.
		public void DeleteOldUpdaterCopies()
		{
			try
			{
				if (!Directory.Exists(TempDir)) return;
				foreach (string file in Directory.EnumerateFiles(TempDir, "RebornUpdater-*.exe"))
				{
					if (!UpdaterCopyName().IsMatch(Path.GetFileName(file))) continue;
					try { File.Delete(file); }
					catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
				}
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
		}

		[GeneratedRegex(@"^RebornUpdater-[0-9a-f]{32}\.exe\z")]
		private static partial Regex UpdaterCopyName();

		// RebornUpdater.exe's command lines (Task 6). Every path is absolute; Windows paths can't hold a ".
		public static string InstallArgs(string stagedWin64, string version, string win64, int panelPid, string panelExe) =>
			$"install --staged {Quote(stagedWin64)} --version {version} --win64 {Quote(win64)} --panel-pid {panelPid} --panel {Quote(panelExe)}";

		// version must pass IsSafeVersionName (checked again here, so nothing else can reach the command line).
		public static string RollbackArgs(string version, string win64, int panelPid, string panelExe) =>
			IsSafeVersionName(version)
				? $"rollback --to \"{version}\" --win64 {Quote(win64)} --panel-pid {panelPid} --panel {Quote(panelExe)}"
				: throw new ArgumentException("Not a saved version name: " + version, nameof(version));

		// The Installer's rule for a saved version's folder name: letters, digits, '.', '_' and '-', starting
		// with a letter or digit, at most 80 long, and not the staging folder.
		public static bool IsSafeVersionName(string? name) =>
			!string.IsNullOrEmpty(name)
			&& name.Length <= 80
			&& SafeName().IsMatch(name)
			&& !string.Equals(name, StagingFolder, StringComparison.OrdinalIgnoreCase);

		[GeneratedRegex(@"^[A-Za-z0-9][A-Za-z0-9._-]*\z")]
		private static partial Regex SafeName();

		// No trailing backslash: it would escape the closing quote.
		private static string Quote(string path) => "\"" + Path.GetFullPath(path).TrimEnd('\\', '/') + "\"";

		// The saved versions a roll back can put back, newest first (not the installed one).
		public IReadOnlyList<string> Kept()
		{
			try { return new Installer(_paths.Win64, NoServers.Instance, () => DateTime.Now).Kept(); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return Array.Empty<string>(); }
		}

		public LastUpdate? ReadLastResult()
		{
			try
			{
				using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(VersionsDir, Installer.LastUpdateFile)));
				JsonElement root = doc.RootElement;
				if (root.ValueKind != JsonValueKind.Object) return null;
				string? message = Text(root, "message");
				if (string.IsNullOrWhiteSpace(message)) return null;
				bool ok = root.TryGetProperty("ok", out JsonElement o) && o.ValueKind == JsonValueKind.True;
				DateTimeOffset? when = DateTimeOffset.TryParse(Text(root, "when"), CultureInfo.InvariantCulture,
					DateTimeStyles.AssumeLocal, out DateTimeOffset w) ? w : null;
				return new LastUpdate(Text(root, "version") ?? "", ok, message, Text(root, "details") ?? "", when);
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
		}

		// The update that broke off, or null. A mark that can't be read, or that names an unsafe version,
		// counts as "from unknown", as the Installer reads it. An unsafe "to" is left out ("").
		public UnfinishedUpdate? ReadUnfinished()
		{
			string path = Path.GetFullPath(Path.Combine(VersionsDir, Installer.InProgressFile));
			if (!File.Exists(path)) return null;
			bool canRollBack;
			try { canRollBack = new Installer(_paths.Win64, NoServers.Instance, () => DateTime.Now).CanRecover(); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { canRollBack = false; }
			try
			{
				using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(path));
				if (doc.RootElement.ValueKind == JsonValueKind.Object && Text(doc.RootElement, "from") is string from
					&& IsSafeVersionName(from))
				{
					string to = Text(doc.RootElement, "to") ?? "";
					return new UnfinishedUpdate(from, IsSafeVersionName(to) ? to : "", canRollBack, path);
				}
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
			return new UnfinishedUpdate("unknown", "", false, path);
		}

		// The updater an update that broke off brought with it (its staged copy), or null. The installed one
		// may be half-copied after such an update.
		public string? StagedUpdaterFor(string version)
		{
			if (!IsSafeVersionName(version)) return null;
			string path = Path.Combine(StagingDir, version, "Win64", "ControlPanel", UpdaterExe);
			return File.Exists(path) ? path : null;
		}

		private static string? Text(JsonElement root, string name) =>
			root.TryGetProperty(name, out JsonElement e) && e.ValueKind == JsonValueKind.String ? e.GetString() : null;

		// Runs gh (with the kept token as GH_TOKEN when there is one). A server with no gh but a token gets the
		// same files straight from GitHub instead. tag: the release (null: the newest), names: the files wanted.
		private Task<(int Exit, string Output)> RunAsync(string args, string? tag, string[] names, string dir) => Task.Run(async () =>
		{
			string? token = _token();
			var env = new Dictionary<string, string>();
			if (token != null) env["GH_TOKEN"] = token;
			int exit = _runner.Run(_gh, args, env, out string output);
			output ??= "";
			if (exit == -1 && output.EndsWith(ProgramPath.NotFoundSuffix, StringComparison.Ordinal) && token != null)
				return await FetchOverHttpAsync(token, tag, names, dir);
			return (exit, output);
		});

		private const string ApiRoot = "https://api.github.com/repos/" + Repo + "/releases/";

		// GitHub's REST interface: the release's asset list, then each wanted asset by name. Exit 0 when all
		// were saved into dir. The token only ever goes into the Authorization header of the requests to GitHub's
		// own address. Messages never carry it.
		private async Task<(int Exit, string Output)> FetchOverHttpAsync(string token, string? tag, string[] names, string dir)
		{
			try
			{
				if (tag != null && !IsSafeVersionName(tag)) return (1, "release not found");
				string url = ApiRoot + (tag == null ? "latest" : "tags/" + Uri.EscapeDataString(tag));
				using HttpResponseMessage release = await SendAsync(url, "application/vnd.github+json", token);
				if (release.StatusCode == HttpStatusCode.NotFound) return (1, "release not found");
				if (!release.IsSuccessStatusCode) return (1, $"GitHub answered {(int)release.StatusCode}.");

				var assets = new Dictionary<string, string>(StringComparer.Ordinal);
				using (JsonDocument doc = JsonDocument.Parse(await release.Content.ReadAsStringAsync()))
				{
					if (doc.RootElement.TryGetProperty("assets", out JsonElement list) && list.ValueKind == JsonValueKind.Array)
						foreach (JsonElement a in list.EnumerateArray())
							if (Text(a, "name") is string n && Text(a, "url") is string u) assets[n] = u;
				}
				foreach (string name in names)
					if (Path.GetFileName(name) != name || !assets.ContainsKey(name)) return (1, "no assets match");

				foreach (string name in names)
				{
					string next = assets[name];
					HttpResponseMessage response = await SendAsync(next, "application/octet-stream", token);
					try
					{
						// Asset requests answer with a redirect to the file host: follow it without the token.
						for (int hops = 0; (int)response.StatusCode is >= 300 and < 400 && response.Headers.Location is Uri to && hops < 5; hops++)
						{
							string target = new Uri(new Uri(next), to).AbsoluteUri;
							response.Dispose();
							response = await SendAsync(target, "application/octet-stream", null);
						}
						if (!response.IsSuccessStatusCode) return (1, $"GitHub answered {(int)response.StatusCode}.");
						await using FileStream file = File.Create(Path.Combine(dir, name));
						await response.Content.CopyToAsync(file);
					}
					finally { response.Dispose(); }
				}
				return (0, "");
			}
			catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException or UnauthorizedAccessException or JsonException)
			{
				return (1, "");
			}
		}

		private Task<HttpResponseMessage> SendAsync(string url, string accept, string? token)
		{
			var request = new HttpRequestMessage(HttpMethod.Get, url);
			request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue(accept));
			request.Headers.UserAgent.ParseAdd("BattlebornRebornControlPanel");
			request.Headers.TryAddWithoutValidation("X-GitHub-Api-Version", "2022-11-28");
			if (token != null) request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
			return _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
		}

		// gh reached GitHub but the release has nothing to download (an old-style release, or none at all).
		private static bool NothingToGet(string output) =>
			output.Contains("no assets match", StringComparison.OrdinalIgnoreCase)
			|| output.Contains("release not found", StringComparison.OrdinalIgnoreCase);

		private static UpdateCheck Problem(string message) => new(CheckOutcome.Problem, null, message);

		private static StagedUpdate Failed(string staging, string message)
		{
			TryDeleteDirectory(staging);
			return new StagedUpdate(false, null, message);
		}

		private static void TryDeleteDirectory(string dir)
		{
			try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); }
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
		}

		// Kept() only lists folders; it never stops or starts anything.
		private sealed class NoServers : IServerControl
		{
			public static readonly NoServers Instance = new();
			public void StopAll() => throw new InvalidOperationException("The panel never stops servers itself.");
			public bool StartMatchmaking() => throw new InvalidOperationException("The panel never starts servers here.");
			public bool WaitForGame(TimeSpan timeout) => throw new InvalidOperationException("The panel never waits for games here.");
		}
	}
}
