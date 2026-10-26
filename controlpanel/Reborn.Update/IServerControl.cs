namespace Reborn.Update;

/// <summary>Starts and stops the game servers on the VM. The real one lives in RebornUpdater.exe;
/// tests use a fake that records calls.</summary>
public interface IServerControl
{
    /// <summary>Stops every game server and matchmaking.</summary>
    void StopAll();

    /// <summary>Starts matchmaking. False when it would not start.</summary>
    bool StartMatchmaking();

    /// <summary>Waits until matchmaking lists at least one game. False when none shows up in time.</summary>
    bool WaitForGame(TimeSpan timeout);
}
