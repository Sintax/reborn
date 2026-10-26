using System.IO;
using System.Windows;
using ControlPanel.Core;

namespace ControlPanel
{
	// This panel's own process, for the updater: its id, its exe, and closing it.
	public sealed class WpfPanelApp : IPanelApp
	{
		public int ProcessId => Environment.ProcessId;

		public string ExePath => Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "ControlPanel.exe");

		// The updater waits up to 30 s for the panel to end, so it shuts down at once.
		public void Close() => Application.Current?.Shutdown();
	}
}
