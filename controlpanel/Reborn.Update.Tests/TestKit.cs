namespace Reborn.Update.Tests;

/// <summary>Pretend servers: records every call, what reborn.dll and reborn-deployed.txt held at that
/// moment (null = missing), and answers from queues (empty queue = true).</summary>
public sealed class FakeServers(string win64) : IServerControl
{
    public sealed record Seen(string Call, string? Dll, string? Deployed);

    public List<Seen> Snapshots { get; } = new();
    public List<string> Calls => Snapshots.Select(s => s.Call).ToList();
    public Queue<bool> StartAnswers { get; } = new();
    public Queue<bool> WaitAnswers { get; } = new();
    public int StopThrows { get; set; }
    public TimeSpan? LastTimeout { get; private set; }

    public void Clear() => Snapshots.Clear();
    public Seen First(string call) => Snapshots.First(s => s.Call == call);

    public void StopAll()
    {
        Note("StopAll");
        if (StopThrows > 0) { StopThrows--; throw new InvalidOperationException("Serverborn.exe would not close."); }
    }

    public bool StartMatchmaking()
    {
        Note("StartMatchmaking");
        return StartAnswers.Count == 0 || StartAnswers.Dequeue();
    }

    /// <summary>Runs inside WaitForGame, before it answers.</summary>
    public Action? OnWait { get; set; }

    public bool WaitForGame(TimeSpan timeout)
    {
        Note("WaitForGame");
        LastTimeout = timeout;
        OnWait?.Invoke();
        return WaitAnswers.Count == 0 || WaitAnswers.Dequeue();
    }

    void Note(string call) => Snapshots.Add(new Seen(call, ReadOrNull("reborn.dll"), ReadOrNull("reborn-deployed.txt")));

    string? ReadOrNull(string relative)
    {
        string path = Path.Combine(win64, relative);
        return File.Exists(path) ? File.ReadAllText(path) : null;
    }
}

/// <summary>A throw-away folder holding a pretend Win64 and staged updates. Deleted afterwards.</summary>
public sealed class Scratch : IDisposable
{
    public string Root { get; } = Path.Combine(Path.GetTempPath(), "reborn-update-tests", Guid.NewGuid().ToString("N"));
    public string Win64 => Path.Combine(Root, "Win64");

    public Scratch() => Directory.CreateDirectory(Win64);

    public string Staged(string name)
    {
        string dir = Path.Combine(Root, "staged-" + name);
        Directory.CreateDirectory(dir);
        return dir;
    }

    public static void Write(string dir, string relative, string text)
    {
        string path = Path.Combine(dir, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    public string Read(string relative) => File.ReadAllText(Path.Combine(Win64, relative));
    public bool Has(string relative) => File.Exists(Path.Combine(Win64, relative));
    public string Versions => Path.Combine(Win64, "RebornVersions");

    public void Dispose()
    {
        try { Directory.Delete(Root, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
}
