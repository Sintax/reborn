namespace Reborn.Update.Tests;

/// <summary>Pretend servers: records every call and answers from queues (empty queue = true).</summary>
public sealed class FakeServers : IServerControl
{
    public List<string> Calls { get; } = new();
    public Queue<bool> StartAnswers { get; } = new();
    public Queue<bool> WaitAnswers { get; } = new();
    public TimeSpan? LastTimeout { get; private set; }

    public void StopAll() => Calls.Add("StopAll");

    public bool StartMatchmaking()
    {
        Calls.Add("StartMatchmaking");
        return StartAnswers.Count == 0 || StartAnswers.Dequeue();
    }

    public bool WaitForGame(TimeSpan timeout)
    {
        Calls.Add("WaitForGame");
        LastTimeout = timeout;
        return WaitAnswers.Count == 0 || WaitAnswers.Dequeue();
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
