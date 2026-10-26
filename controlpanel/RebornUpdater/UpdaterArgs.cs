using System.Globalization;
using Reborn.Update;

namespace RebornUpdater;

/// <summary>The command line of RebornUpdater.exe. Pure parsing, so tests can run it without the exe.</summary>
public sealed record UpdaterArgs(string Command, string Win64, int PanelPid, string Panel, string Version, string? Staged)
{
    public const string Install = "install";
    public const string Rollback = "rollback";

    public static readonly string Usage = string.Join(Environment.NewLine,
        "RebornUpdater.exe is started by the Control Panel. It is not meant to be run by hand.",
        "",
        "  RebornUpdater.exe install --staged <folder> --version <1.0.N> --win64 <folder> --panel-pid <number> --panel <exe>",
        "  RebornUpdater.exe rollback --to <version> --win64 <folder> --panel-pid <number> --panel <exe>",
        "",
        "  --staged     the folder with the new server files (the update's Win64 folder)",
        "  --version    the release being installed, like 1.0.7",
        "  --to         the saved version to go back to",
        "  --win64      the game's Binaries\\Win64 folder",
        "  --panel-pid  the Control Panel's process number; the updater waits for it to close",
        "  --panel      the Control Panel program to start again when done");

    /// <summary>Returns null when the command line is not exactly one of the two forms above.
    /// For a roll back, <see cref="Version"/> holds the version from --to.</summary>
    public static UpdaterArgs? Parse(string[]? args)
    {
        if (args is null || args.Length == 0) return null;
        string command = args[0];
        if (command != Install && command != Rollback) return null;
        if (args.Length % 2 != 1) return null;

        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (int i = 1; i < args.Length; i += 2)
        {
            string flag = args[i], value = args[i + 1];
            if (!flag.StartsWith("--", StringComparison.Ordinal)) return null;
            if (string.IsNullOrWhiteSpace(value)) return null;
            if (!values.TryAdd(flag, value)) return null;   // the same flag twice
        }

        string[] allowed = command == Install
            ? ["--staged", "--version", "--win64", "--panel-pid", "--panel"]
            : ["--to", "--win64", "--panel-pid", "--panel"];
        if (values.Count != allowed.Length || values.Keys.Any(k => !allowed.Contains(k))) return null;

        if (!int.TryParse(values["--panel-pid"], NumberStyles.None, CultureInfo.InvariantCulture, out int pid) || pid <= 0)
            return null;

        if (command == Install)
        {
            string version = values["--version"].Trim();
            if (Versions.Parse(version) == 0) return null;
            return new UpdaterArgs(command, values["--win64"], pid, values["--panel"], version, values["--staged"]);
        }
        return new UpdaterArgs(command, values["--win64"], pid, values["--panel"], values["--to"].Trim(), null);
    }
}
