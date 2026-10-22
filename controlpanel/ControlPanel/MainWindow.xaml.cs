using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using ControlPanel.Core.ViewModels;

namespace ControlPanel
{
	// Wiring only: the refresh timer and loading the log when the Logs tab opens.
	public partial class MainWindow : Window
	{
		private readonly MainViewModel _main;
		private readonly DispatcherTimer _timer;
		private bool _refreshing;

		public MainWindow(MainViewModel main)
		{
			InitializeComponent();
			_main = main;
			DataContext = main;
			_timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = MainViewModel.RefreshInterval };
			_timer.Tick += async (_, _) => await RefreshAsync();
			Loaded += async (_, _) =>
			{
				await RefreshAsync();
				_timer.Start();
			};
			Closed += (_, _) => _timer.Stop();
		}

		// Never overlaps (a slow matchmaking can take longer than the interval) and never lets an
		// error out of the timer: the next tick simply tries again.
		private async Task RefreshAsync()
		{
			if (_refreshing) return;
			_refreshing = true;
			try { await _main.RefreshAsync(); }
			catch (Exception) { }
			finally { _refreshing = false; }
		}

		private void Tabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			// Lists inside the pages raise this too; only the tabs themselves count.
			if (!ReferenceEquals(e.OriginalSource, Tabs) || !LogsTab.IsSelected) return;
			try { _main.Logs.RefreshCommand.Execute(null); }
			catch (Exception) { }
		}
	}
}
