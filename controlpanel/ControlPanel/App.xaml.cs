using System.Windows;
using System.Windows.Threading;
using ControlPanel.Core;
using ControlPanel.Core.ViewModels;

namespace ControlPanel
{
	// Puts the panel together: paths, the matchmaking client, the Windows services, the window.
	public partial class App : Application
	{
		private DateTime _lastErrorShownUtc = DateTime.MinValue;

		protected override void OnStartup(StartupEventArgs e)
		{
			base.OnStartup(e);

			// Nothing that goes wrong may close the panel: say it plainly and carry on.
			DispatcherUnhandledException += OnUnhandled;
			TaskScheduler.UnobservedTaskException += (_, args) => args.SetObserved();

			PanelPaths paths = PanelPaths.ForThisApp();
			var main = new MainViewModel(AdminClient.For(paths), paths, new WindowsClipboard(), new WindowsProcessRunner(),
				new WpfDialogs(), new WindowsMemory());
			var window = new MainWindow(main);
			MainWindow = window;
			window.Show();
		}

		private void OnUnhandled(object sender, DispatcherUnhandledExceptionEventArgs e)
		{
			e.Handled = true;
			// One box at a time, and not again within a few seconds (a repeating error would flood).
			DateTime now = DateTime.UtcNow;
			if (now - _lastErrorShownUtc < TimeSpan.FromSeconds(10)) return;
			_lastErrorShownUtc = now;
			MessageBox.Show(
				"Something went wrong: " + e.Exception.Message + "\n\nThe Control Panel keeps running. If this keeps happening, close it and open it again.",
				WpfDialogs.Caption, MessageBoxButton.OK, MessageBoxImage.Warning);
			_lastErrorShownUtc = DateTime.UtcNow;
		}
	}
}
