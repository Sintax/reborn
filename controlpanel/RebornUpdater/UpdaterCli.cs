using System.Diagnostics;
using System.Text.Json;
using Reborn.Update;

namespace RebornUpdater;

/// <summary>What RebornUpdater.exe does, with the outside world handed in so tests can run it.
/// Exit codes: 0 done, 1 the update or roll back did not work (the old version is back, or nothing was
/// touched), 2 bad command line.</summary>
public static class UpdaterCli
{
    public const string PanelStillOpen = "Close the Control Panel and try again.";
    public static readonly TimeSpan PanelWait = TimeSpan.FromSeconds(30);

    /// <param name="servers">Starts and stops the real servers. Null = this machine's.</param>
    /// <param name="panelGone">Waits up to the given time for the process to end. True = it has.</param>
    /// <param name="startPanel">Starts the program (path, working folder). True = it started.</param>
    public static int Run(string[] args, TextWriter output, IServerControl? servers = null,
        Func<int, TimeSpan, bool>? panelGone = null, Func<string, string, bool>? startPanel = null, Func<DateTime>? now = null)
    {
        UpdaterArgs? a = UpdaterArgs.Parse(args);
        if (a is null)
        {
            output.WriteLine(UpdaterArgs.Usage);
            return 2;
        }

        now ??= () => DateTime.Now;
        panelGone ??= WaitForProcessToEnd;
        startPanel ??= StartProgram;

        // Everything downstream gets absolute paths, whatever folder the updater was started from.
        string win64, panel, staged;
        try
        {
            win64 = Path.GetFullPath(a.Win64);
            panel = Path.GetFullPath(a.Panel);
            staged = a.Staged is null ? "" : Path.GetFullPath(a.Staged);
        }
        catch (Exception e)
        {
            output.WriteLine("The updater hit a problem: " + e.Message);
            return 1;
        }

        // Check the folders before anything is closed or stopped.
        if (!Directory.Exists(win64) || !(File.Exists(Path.Combine(win64, "Battleborn.exe")) || Directory.Exists(Path.Combine(win64, "matchmaking"))))
        {
            output.WriteLine($"{win64} isn't the game's Win64 folder (no Battleborn.exe or matchmaking folder there). Nothing was changed.");
            return 1;
        }
        if (a.Command == UpdaterArgs.Install && !Directory.Exists(staged))
        {
            output.WriteLine("The downloaded update folder is missing. Nothing was changed.");
            return 1;
        }

        string what = a.Command == UpdaterArgs.Install ? $"Updating the server to {a.Version}" : $"Going back to {a.Version}";
        bool panelClosed = false;
        int exit = 1;
        try
        {
            output.WriteLine($"{what}.");
            output.WriteLine("Waiting for the Control Panel to close...");
            if (!panelGone(a.PanelPid, PanelWait))
            {
                output.WriteLine(PanelStillOpen + " Nothing was changed.");
                WriteResult(win64, a.Version, false, PanelStillOpen, now);
                return 1;
            }
            panelClosed = true;

            InstallResult result;
            try
            {
                var installer = new Installer(win64, servers ?? new WindowsServerControl(win64, output.WriteLine), now);
                output.WriteLine("Saving a copy of the current files, then stopping the game servers and matchmaking.");
                output.WriteLine("Then the new files go in and matchmaking starts. This can take up to 3 minutes. Please wait.");
                result = a.Command == UpdaterArgs.Install ? installer.Install(staged, a.Version) : installer.RollBack(a.Version);
            }
            catch (Exception e)
            {
                result = new InstallResult(false, $"{StoppedText(a)} because of an error: {e.Message}");
                WriteResult(win64, a.Version, false, result.Message, now);
            }

            output.WriteLine(result.Message);
            output.WriteLine(result.Ok ? "Done." : "This did not work.");
            exit = result.Ok ? 0 : 1;
        }
        catch (Exception e)
        {
            output.WriteLine("The updater hit a problem: " + e.Message);
            exit = 1;
        }
        finally
        {
            // The panel was closed for this run, so open it again whatever happened.
            if (panelClosed)
            {
                try
                {
                    output.WriteLine("Opening the Control Panel again...");
                    if (!startPanel(panel, Path.GetDirectoryName(panel) ?? win64))
                        output.WriteLine("The Control Panel could not be opened. Start it yourself from " + panel);
                }
                catch (Exception e)
                {
                    try { output.WriteLine("The Control Panel could not be opened (" + e.Message + "). Start it yourself from " + panel); }
                    catch (Exception) { }
                }
            }
        }
        return exit;
    }

    /// <summary>The start of the plain sentence for a run that stopped on an error.</summary>
    public static string StoppedText(UpdaterArgs a) =>
        a.Command == UpdaterArgs.Install ? "The update stopped" : "The roll back stopped";

    // Same shape as the installer's own record, for the cases where it is never called.
    static void WriteResult(string win64, string version, bool ok, string message, Func<DateTime> now)
    {
        try
        {
            string dir = Path.Combine(win64, Installer.VersionsFolder);
            Directory.CreateDirectory(dir);
            string json = JsonSerializer.Serialize(new { version, ok, message, details = "", when = now() },
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(Path.Combine(dir, Installer.LastUpdateFile), json);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    static bool WaitForProcessToEnd(int pid, TimeSpan limit)
    {
        Process p;
        try { p = Process.GetProcessById(pid); }
        catch (ArgumentException) { return true; }   // no such process: it is already gone
        catch (Exception) { return false; }
        using (p)
        {
            try { return p.WaitForExit(limit); }
            catch (Exception) { return false; }
        }
    }

    static bool StartProgram(string exe, string workingDir)
    {
        try
        {
            using Process? p = Process.Start(new ProcessStartInfo { FileName = exe, WorkingDirectory = workingDir, UseShellExecute = true });
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
