using System.Net;
using System.Text;
using System.Text.Json;
using ControlPanel.Core;
using ControlPanel.Core.ViewModels;

namespace ControlPanel.Tests
{
	// A pretend matchmaking admin API: answers the routes with the JSON shapes of
	// gamecontroller/Controllers/Admin.cs and records every request it was sent.
	public sealed class FakeApi : HttpMessageHandler
	{
		public sealed record Sent(string Method, string Path, string? Body, string? Token);

		public bool Down { get; set; }
		public List<Sent> Requests { get; } = new();

		// Return a response here to answer a request differently (null = the normal answer).
		public Func<Sent, HttpResponseMessage?>? Answer { get; set; }

		public object Status { get; set; } = new
		{
			version = "1.0.7",
			publicIp = "203.0.113.10",
			gamePorts = new[] { 7777, 7778 },
			gamePortsInUse = new[] { 7777, 7778 },
			gamesRunning = 0,
			requireKeys = true,
			matchmakingStartedUtc = "2026-10-22T11:22:35.06Z",
		};

		public object Games { get; set; } = Array.Empty<object>();

		public object Users { get; set; } = new
		{
			requireKeys = true,
			users = new object[]
			{
				new { name = "Sintax", canCreateGames = true, confirmName = false, added = "2026-10-20T10:00:00Z", lastSeen = "2026-10-22T09:00:00Z" },
				new { name = "dj", canCreateGames = true, confirmName = true, added = (string?)null, lastSeen = (string?)null },
				new { name = "Kaz", canCreateGames = false, confirmName = false, added = "2026-10-21T10:00:00Z", lastSeen = (string?)null },
			},
		};

		public IEnumerable<Sent> Writes => Requests.Where(r => r.Method != "GET");

		protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
		{
			if (Down) throw new HttpRequestException("No connection could be made because the target machine actively refused it.");
			string? body = request.Content == null ? null : await request.Content.ReadAsStringAsync(cancellationToken);
			string? token = request.Headers.TryGetValues("X-Admin-Token", out var values) ? values.Single() : null;
			var sent = new Sent(request.Method.Method, request.RequestUri!.AbsolutePath, body, token);
			Requests.Add(sent);
			if (Answer?.Invoke(sent) is HttpResponseMessage special) return special;

			const string root = "/api/admin/";
			string route = sent.Path.StartsWith(root) ? sent.Path[root.Length..] : "?";
			return (sent.Method, route) switch
			{
				("GET", "status") => Json(200, Status),
				("GET", "games") => Json(200, Games),
				("GET", "users") => Json(200, Users),
				("GET", _) when route.EndsWith("/key") => Json(200, new { key = "ABCDEFGHJKMNPQRSTVWXYZ23" }),
				("POST", "users") => Json(201, new { name = "Pip", canCreateGames = false, confirmName = false, added = "2026-10-22T12:00:00Z", lastSeen = (string?)null }),
				("PUT", "require-keys") => Json(200, new { requireKeys = body!.Contains("true") }),
				("PUT", "ports") => Json(200, new { restartNeeded = true }),
				("PUT", _) when route.EndsWith("/key") => new HttpResponseMessage(HttpStatusCode.NoContent),
				("PUT", _) => Json(200, new { name = "Kaz", canCreateGames = true, confirmName = false, added = "2026-10-21T10:00:00Z", lastSeen = (string?)null }),
				("DELETE", _) => new HttpResponseMessage(HttpStatusCode.NoContent),
				_ => new HttpResponseMessage(HttpStatusCode.NotFound),
			};
		}

		public static HttpResponseMessage Json(int status, object value) => new((HttpStatusCode)status)
		{
			Content = new StringContent(JsonSerializer.Serialize(value), Encoding.UTF8, "application/json"),
		};

		public static HttpResponseMessage Message(int status, string message) => Json(status, new { message });
	}

	public sealed class FakeClipboard : IClipboard
	{
		public string? Text { get; private set; }

		// When set, SetText fails as Windows does when another program holds the clipboard.
		public bool Busy { get; set; }

		public void SetText(string text)
		{
			if (Busy) throw new System.Runtime.InteropServices.ExternalException("OpenClipboard failed");
			Text = text;
		}
	}

	// A clock the test moves by hand.
	public sealed class ManualClock : TimeProvider
	{
		public DateTimeOffset Now { get; set; } = new(2026, 10, 22, 12, 0, 0, TimeSpan.Zero);
		public override DateTimeOffset GetUtcNow() => Now;
		public override TimeZoneInfo LocalTimeZone => ZoneThrows ? throw new InvalidOperationException("no time zone") : TimeZoneInfo.Utc;
		// Makes reading the local time zone fail, so a reload that shows a dated result throws.
		public bool ZoneThrows { get; set; }
	}

	public sealed class FakeRunner : IProcessRunner
	{
		public List<string> Started { get; } = new();
		public List<string> Elevated { get; } = new();
		public List<string> Stopped { get; } = new();
		public List<string> Opened { get; } = new();
		public HashSet<string> Running { get; } = new(StringComparer.OrdinalIgnoreCase);
		public int ElevatedExitCode { get; set; }

		// When set, RunElevated waits for this instead of answering at once.
		public TaskCompletionSource<int>? ElevatedGate { get; set; }

		// Breaks the "never throws" promise, as a buggy window project might.
		public bool Throws { get; set; }

		private void MaybeThrow()
		{
			if (Throws) throw new System.ComponentModel.Win32Exception(5, "Access is denied");
		}

		// Every Start in full, and the answer Start gives.
		public List<(string Exe, string Args, string WorkingDir, bool Minimized)> StartedWith { get; } = new();
		public bool StartWorks { get; set; } = true;

		public bool Start(string exe, string args, string workingDir, bool minimized = true)
		{
			MaybeThrow();
			Started.Add(exe);
			StartedWith.Add((exe, args, workingDir, minimized));
			return StartWorks;
		}

		// Every Run as "file args". OnRun answers it (exit code and output); by default a program that
		// cannot start (-1).
		public List<string> Ran { get; } = new();
		public Func<string, string, (int Exit, string Output)>? OnRun { get; set; }

		// The environment every Run was given (empty when none).
		public List<IReadOnlyDictionary<string, string>> Environments { get; } = new();

		public int Run(string file, string args, out string output) => Run(file, args, new Dictionary<string, string>(), out output);

		public int Run(string file, string args, IReadOnlyDictionary<string, string> environment, out string output)
		{
			MaybeThrow();
			lock (Ran) { Ran.Add(file + " " + args); Environments.Add(environment); }
			(int exit, output) = OnRun?.Invoke(file, args) ?? (-1, "");
			return exit;
		}

		public Task<int> RunElevated(string exe, string args)
		{
			MaybeThrow();
			Elevated.Add(exe + " " + args);
			return ElevatedGate?.Task ?? Task.FromResult(ElevatedExitCode);
		}

		public bool IsRunning(string processName)
		{
			MaybeThrow();
			return Running.Contains(processName);
		}

		public void StopAll(string processName)
		{
			MaybeThrow();
			Stopped.Add(processName);
			Running.Remove(processName);
		}

		public void OpenFolder(string path) => Opened.Add(path);
	}

	public sealed class FakeDialogs : IDialogs
	{
		public bool ConfirmAnswer { get; set; }
		public string? TextAnswer { get; set; }
		public List<string> Questions { get; } = new();

		// Runs while the question is "on screen" (a real message box keeps the timer ticking).
		public Action<string>? WhileAsking { get; set; }

		public bool Confirm(string question)
		{
			Questions.Add(question);
			WhileAsking?.Invoke(question);
			return ConfirmAnswer;
		}

		public string? AskText(string question, string startText)
		{
			Questions.Add(question);
			return TextAnswer;
		}
	}

	public sealed class FakeApp : IPanelApp
	{
		public int ProcessId { get; set; } = 4242;
		public string ExePath { get; set; } = @"C:\Panel\ControlPanel.exe";
		public int Closed { get; private set; }
		public void Close() => Closed++;
	}

	public sealed class FakeMemory : ISystemMemory
	{
		public MemoryReading? Reading { get; set; } = new(16.0, 9.5);
		public MemoryReading? Read() => Reading;
	}

	// A throwaway game folder (Binaries\Win64 with matchmaking\) and user profile in %TEMP%.
	public sealed class TempPanel : IDisposable
	{
		public string Root { get; }
		public string Win64 { get; }
		public string Profile { get; }
		public PanelPaths Paths { get; }
		public FakeApi Api { get; } = new();
		public FakeClipboard Clipboard { get; } = new();
		public FakeRunner Runner { get; } = new();
		public FakeDialogs Dialogs { get; } = new();
		public FakeMemory Memory { get; } = new();
		public FakeApp App { get; } = new();

		public TempPanel(string? settings = null)
		{
			Root = Path.Combine(Path.GetTempPath(), "cp-tests-" + Guid.NewGuid().ToString("N"));
			Win64 = Path.Combine(Root, "Battleborn", "Binaries", "Win64");
			Profile = Path.Combine(Root, "profile");
			Directory.CreateDirectory(Path.Combine(Win64, "matchmaking"));
			Directory.CreateDirectory(Path.Combine(Win64, "ControlPanel"));
			Directory.CreateDirectory(Profile);
			File.WriteAllText(Path.Combine(Win64, "Battleborn.exe"), "not really the game");
			File.WriteAllText(Path.Combine(Win64, "matchmaking", "gamecontroller.exe"), "not really matchmaking");
			File.WriteAllText(Path.Combine(Win64, "matchmaking", "appsettings.json"),
				settings ?? "{\n  // comment\n  \"Urls\": \"http://0.0.0.0:5000\",\n  \"CoordinatorPort\": \"5000\",\n  \"GamePorts\": [ 7777, 7778 ]\n}\n");
			File.WriteAllText(Path.Combine(Win64, "matchmaking", "admin-token.txt"), "TOKENTOKENTOKENTOKENTOKENTOKEN22\r\n");
			Paths = new PanelPaths(Path.Combine(Win64, "ControlPanel"), Profile);
		}

		public AdminClient Client() => new(new HttpClient(Api), Paths.ReadToken, Paths.AdminBaseUri);

		public MainViewModel Main(TimeProvider? clock = null, UpdateService? updates = null) =>
			new(Client(), Paths, Clipboard, Runner, Dialogs, Memory, App, clock,
				updates ?? new UpdateService(Paths, Runner, tempDir: Path.Combine(Root, "temp"), token: () => null));

		public void Dispose()
		{
			try { Directory.Delete(Root, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
		}
	}
}
