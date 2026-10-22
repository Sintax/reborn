using System.Windows.Controls;

namespace ControlPanel.Views
{
	public partial class LogsView : UserControl
	{
		public LogsView() => InitializeComponent();

		// Newest lines are at the bottom: show them after each refresh.
		private void LogText_TextChanged(object sender, TextChangedEventArgs e) => LogText.ScrollToEnd();
	}
}
