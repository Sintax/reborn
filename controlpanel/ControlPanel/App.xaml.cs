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

			// The installer's hand-over of the GitHub token: no window, nothing shown.
			if (GitHubToken.SwitchFile(e.Args) is string tokenFile)
			{
				Shutdown(GitHubToken.SaveFromFile(tokenFile));
				return;
			}

			// Nothing that goes wrong may close the panel: say it plainly and carry on.
			DispatcherUnhandledException += OnUnhandled;
			TaskScheduler.UnobservedTaskException += (_, args) => args.SetObserved();

			PanelPaths paths = PanelPaths.ForThisApp();
			var runner = new WindowsProcessRunner();
			var main = new MainViewModel(AdminClient.For(paths), paths, new WindowsClipboard(), runner,
				new WpfDialogs(), new WindowsMemory(), new WpfPanelApp(), updates: new UpdateService(paths, runner, TestUpdateKey()));
			var window = new MainWindow(main);
			MainWindow = window;
			window.Show();
		}

		// Null (the release key in UpdateKey.cs) in every Release build, which is what the VM runs. A Debug
		// build may take a throw-away public key from REBORN_UPDATE_TEST_KEY, so a laptop live check can
		// sign a test update without the owner's key.
		private static string? TestUpdateKey()
		{
#if DEBUG
			string? key = Environment.GetEnvironmentVariable("REBORN_UPDATE_TEST_KEY");
			return string.IsNullOrWhiteSpace(key) ? null : key.Trim();
#else
			return null;
#endif
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
