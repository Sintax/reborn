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
	}
}
