using RebornUpdater;

// Started by the Control Panel from a temporary copy, so it can replace ControlPanel\RebornUpdater.exe.
int exit = UpdaterCli.Run(args, Console.Out);

// The window closes the moment this ends. After a problem, leave the last lines up long enough to read,
// unless the output goes to a file or another program.
if (exit != 0 && !Console.IsOutputRedirected)
{
    Console.WriteLine($"This window closes in {UpdaterCli.FailurePause.TotalSeconds:0} seconds.");
    Thread.Sleep(UpdaterCli.FailurePause);
}
return exit;
