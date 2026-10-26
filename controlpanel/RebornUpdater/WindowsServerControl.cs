using System.Diagnostics;
using System.Text.Json;
using Reborn.Update;

namespace RebornUpdater;

/// <summary>Stops and starts the real game servers and matchmaking on this machine, the way
/// StartMatchmaking.bat and StopMatchmaking.bat do. Never throws: anything that goes wrong is a "no".</summary>
public sealed class WindowsServerControl(string win64, Action<string>? note = null) : IServerControl
{
    public const int DefaultCoordinatorPort = 5000;

    /// <summary>Printed when a gamecontroller program is running but its location can't be read.</summary>
    public const string UnreadableNote =
        "NOTE: A gamecontroller program is running but its location can't be read. It may be matchmaking running under another " +
        "Windows account, and it could not be stopped. If the update fails, close it by hand (Task Manager) and try again.";

    bool _noted;
    static readonly TimeSpan StopWait = TimeSpan.FromSeconds(15);
    static readonly TimeSpan PollEvery = TimeSpan.FromSeconds(5);

    string MatchmakingDir => Path.Combine(win64, "matchmaking");
    string MatchmakingExe => Path.Combine(MatchmakingDir, "gamecontroller.exe");

    public void StopAll()
    {
        // Matchmaking first, so it cannot start another game server while they are being closed.
        Stop(MatchmakingProcesses());
        Stop(Find("Serverborn", _ => true));
    }

    public bool StartMatchmaking()
    {
        try
        {
            List<Process> running = MatchmakingProcesses();
            bool alreadyUp = running.Count > 0;
            Release(running);
            if (alreadyUp) return true;
            if (!File.Exists(MatchmakingExe)) return false;

            // The game servers run as Serverborn.exe, a copy of Battleborn.exe.
            string server = Path.Combine(win64, "Serverborn.exe"), game = Path.Combine(win64, "Battleborn.exe");
            if (!File.Exists(server) && File.Exists(game)) File.Copy(game, server);

            using Process? p = Process.Start(new ProcessStartInfo
            {
                FileName = MatchmakingExe,
                WorkingDirectory = MatchmakingDir,
                UseShellExecute = true,
                WindowStyle = ProcessWindowStyle.Minimized,
            });
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    public bool WaitForGame(TimeSpan timeout)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
            string url = $"http://127.0.0.1:{ReadCoordinatorPort(win64)}/api/games";
            DateTime end = DateTime.UtcNow + timeout;
            while (true)
            {
                try
                {
                    if (HasGame(http.GetStringAsync(url).GetAwaiter().GetResult())) return true;
                }
                catch (Exception) { /* not answering yet */ }
                if (DateTime.UtcNow + PollEvery > end) return false;
                Thread.Sleep(PollEvery);
            }
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>"CoordinatorPort" in matchmaking\appsettings.json (a number or a string), else 5000.</summary>
    public static int ReadCoordinatorPort(string win64)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(win64, "matchmaking", "appsettings.json")),
                new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            if (doc.RootElement.ValueKind == JsonValueKind.Object)
            {
                // The name is matched in any letter case, as the matchmaking program's own settings reader does.
                foreach (JsonProperty prop in doc.RootElement.EnumerateObject())
                {
                    if (!string.Equals(prop.Name, "CoordinatorPort", StringComparison.OrdinalIgnoreCase)) continue;
                    JsonElement v = prop.Value;
                    if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) && n is > 0 and <= 65535) return n;
                    if (v.ValueKind == JsonValueKind.String && int.TryParse(v.GetString(), out int s) && s is > 0 and <= 65535) return s;
                }
            }
        }
        catch (Exception) { }
        return DefaultCoordinatorPort;
    }

    /// <summary>True when the games list has at least one game that has started and checked in ("ready": true).</summary>
    public static bool HasGame(string? json) => GamesList.HasReadyGame(json);

    List<Process> MatchmakingProcesses() => Find("gamecontroller", IsMatchmaking);

    // Only the gamecontroller.exe that lives in <Win64>\matchmaking.
    bool IsMatchmaking(Process p)
    {
        try
        {
            string? exe = p.MainModule?.FileName;
            return exe != null && string.Equals(Path.GetFullPath(exe), Path.GetFullPath(MatchmakingExe), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception)
        {
            // Cannot see its path (another account's process, usually): not ours to stop. Say so once.
            if (!_noted)
            {
                _noted = true;
                try { note?.Invoke(UnreadableNote); } catch (Exception) { }
            }
            return false;
        }
    }

    static List<Process> Find(string name, Func<Process, bool> keep)
    {
        var result = new List<Process>();
        Process[] all;
        try { all = Process.GetProcessesByName(name); }
        catch (Exception) { return result; }
        foreach (Process p in all)
        {
            if (keep(p)) result.Add(p);
            else p.Dispose();
        }
        return result;
    }

    static void Stop(List<Process> processes)
    {
        foreach (Process p in processes)
        {
            try { p.Kill(entireProcessTree: true); }
            catch (Exception) { /* already gone, or not ours to stop */ }
        }
        foreach (Process p in processes)
        {
            try { p.WaitForExit(StopWait); }
            catch (Exception) { }
        }
        Release(processes);
    }

    static void Release(List<Process> processes)
    {
        foreach (Process p in processes)
        {
            try { p.Dispose(); } catch (Exception) { }
        }
    }
}
