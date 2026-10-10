using System.Diagnostics;
using System.Text;
using gamecontroller.Singletons;

namespace gamecontroller.Services
{
	// Starts Serverborn.exe the way StartServer.ps1 does: through the Steam loader, from a folder
	// of its own under Win64\rb_ids\<instance> so each server has its own Steam identity and ini.
	public class LoaderServerLauncher : IServerLauncher
	{
		private static readonly string[] LoaderFiles = { "steamclient_loader_x64.exe", "steamclient.dll", "steamclient64.dll" };
		private const long SteamIdBase = 76561197960265728L;

		private readonly string _win64;
		private readonly ILogger<LoaderServerLauncher> _logger;
		private readonly object _lock = new();

		public LoaderServerLauncher(IConfiguration config, ILogger<LoaderServerLauncher> logger)
		{
			// The server kit puts the service in Win64\matchmaking; appsettings.json "Win64" can say otherwise.
			string here = AppContext.BaseDirectory.TrimEnd('\\', '/');
			string parent = Path.GetDirectoryName(here) ?? here;
			_win64 = config["Win64"] ?? (File.Exists(Path.Combine(here, "Battleborn.exe")) ? here : parent);
			_logger = logger;
		}

		public int Launch(string instanceName, string commandLine)
		{
			lock (_lock)
			{
				string idDir = PrepareFiles(instanceName, commandLine);
				HashSet<int> before = Process.GetProcessesByName("Serverborn").Select(p => p.Id).ToHashSet();

				var loader = Process.Start(new ProcessStartInfo(Path.Combine(idDir, LoaderFiles[0]))
				{
					WorkingDirectory = idDir,
					UseShellExecute = false,
					WindowStyle = ProcessWindowStyle.Minimized,
				}) ?? throw new InvalidOperationException("Could not start the Steam loader for " + instanceName);

				// The loader starts the game as a child; wait for that process and track it, not the loader.
				var deadline = DateTime.UtcNow.AddSeconds(60);
				while (DateTime.UtcNow < deadline)
				{
					Process? game = Process.GetProcessesByName("Serverborn").FirstOrDefault(p => !before.Contains(p.Id));
					if (game != null)
					{
						_logger.LogInformation("{Instance}: Serverborn.exe started (pid {Pid})", instanceName, game.Id);
						return game.Id;
					}
					Thread.Sleep(500);
				}

				try { loader.Kill(); } catch { }
				throw new InvalidOperationException("Serverborn.exe for " + instanceName + " did not start within 60 s. Is Steam open on this machine?");
			}
		}

		public bool IsRunning(int pid)
		{
			try
			{
				using var p = Process.GetProcessById(pid);
				return !p.HasExited && p.ProcessName.Equals("Serverborn", StringComparison.OrdinalIgnoreCase);
			}
			catch (ArgumentException) { return false; }
		}

		public void Kill(int pid)
		{
			try
			{
				using var p = Process.GetProcessById(pid);
				p.Kill(entireProcessTree: true);
			}
			catch (ArgumentException) { }
			catch (InvalidOperationException) { }
		}

		private string PrepareFiles(string instanceName, string commandLine)
		{
			string game = Path.Combine(_win64, "Battleborn.exe"), server = Path.Combine(_win64, "Serverborn.exe");
			if (!File.Exists(game)) throw new FileNotFoundException("Battleborn.exe is not in " + _win64 + ". Set \"Win64\" in appsettings.json to the game's Binaries\\Win64 folder.");
			CopyIfChanged(game, server);

			string idDir = Path.Combine(_win64, "rb_ids", instanceName);
			Directory.CreateDirectory(Path.Combine(idDir, "steam_settings"));
			Directory.CreateDirectory(Path.Combine(idDir, "saves"));
			foreach (string f in LoaderFiles)
			{
				if (!File.Exists(Path.Combine(_win64, f))) throw new FileNotFoundException(f + " is missing from " + _win64 + ". Antivirus often removes it: allow it in Windows Security.");
				CopyIfChanged(Path.Combine(_win64, f), Path.Combine(idDir, f));
			}
			string settings = Path.Combine(_win64, "steam_settings");
			if (Directory.Exists(settings)) CopyTree(settings, Path.Combine(idDir, "steam_settings"));

			WriteNoBom(Path.Combine(idDir, "ColdClientLoader.ini"), string.Join("\r\n", new[]
			{
				"[SteamClient]",
				"Exe=" + server,
				"ExeRunDir=" + _win64,
				"ExeCommandLine=" + commandLine,
				"AppId=394230",
				"SteamClientDll=" + Path.Combine(idDir, "steamclient.dll"),
				"SteamClient64Dll=" + Path.Combine(idDir, "steamclient64.dll"),
				"[Injection]", "ForceInjectSteamClient=0", "ForceInjectGameOverlayRenderer=0", "DllsToInjectFolder=",
				"IgnoreInjectionError=1", "IgnoreLoaderArchDifference=0",
				"[Persistence]", "Mode=0",
				"[Debug]", "ResumeByDebugger=0", "",
			}));

			// A stable per-instance Steam id, the same way debugloop/launch.py makes one.
			long steamId = SteamIdBase + (Crc32(instanceName) % 100000);
			WriteNoBom(Path.Combine(idDir, "steam_settings", "configs.user.ini"), string.Join("\r\n", new[]
			{
				"[user::general]",
				"account_name=" + instanceName,
				"account_steamid=" + steamId,
				"language=english",
				"[user::saves]",
				"local_save_path=" + Path.Combine(idDir, "saves"), "",
			}));
			return idDir;
		}

		private static void CopyIfChanged(string from, string to)
		{
			if (File.Exists(to) && new FileInfo(from).Length == new FileInfo(to).Length
				&& File.GetLastWriteTimeUtc(from) == File.GetLastWriteTimeUtc(to)) return;
			File.Copy(from, to, overwrite: true);
		}

		private static void CopyTree(string from, string to)
		{
			Directory.CreateDirectory(to);
			foreach (string dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories))
				Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
			foreach (string file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
				File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), overwrite: true);
		}

		// The loader reads its ini as plain text; a byte-order mark would spoil the first line.
		private static void WriteNoBom(string path, string text) => File.WriteAllText(path, text, new UTF8Encoding(false));

		private static uint Crc32(string s)
		{
			uint crc = 0xFFFFFFFF;
			foreach (byte b in Encoding.UTF8.GetBytes(s))
			{
				crc ^= b;
				for (int i = 0; i < 8; i++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
			}
			return ~crc;
		}
	}
}
