namespace ControlPanel.Core
{
	// The Windows Firewall rule that lets players reach the game servers. The same rule, by the same
	// name, as tools/serverkit_files/OpenFirewall.bat, with the panel's own list of UDP ports.
	public static class Firewall
	{
		public const string RuleName = "Battleborn Reborn server";

		// The arguments for two netsh runs, in order: remove the old rule, then add the new one.
		// The first one fails harmlessly when there is no rule yet.
		public static string[] ArgsFor(IEnumerable<int> ports, string serverExe)
		{
			string list = string.Join(",", ports.Distinct());
			return new[]
			{
				$"advfirewall firewall delete rule name=\"{RuleName}\"",
				$"advfirewall firewall add rule name=\"{RuleName}\" dir=in action=allow profile=any program=\"{serverExe}\" protocol=UDP localport={list} edge=no",
			};
		}

		// The arguments for ONE cmd.exe run that does both netsh commands, so the owner answers one
		// Windows question and the rule can't be deleted without the add being tried right after.
		// /s makes cmd strip only the outer quotes, so the quoted name and path reach netsh unchanged;
		// /d skips any AutoRun command. The exit code is the add's.
		// Null when the program path holds a character cmd would act on even inside quotes (% or ").
		public static string? CommandLine(IEnumerable<int> ports, string serverExe)
		{
			if (serverExe.Contains('%') || serverExe.Contains('"')) return null;
			string[] args = ArgsFor(ports, serverExe);
			return $"/d /s /c \"netsh {args[0]} & netsh {args[1]}\"";
		}
	}
}
