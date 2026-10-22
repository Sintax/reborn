using System.Windows;
using ControlPanel.Core;

namespace ControlPanel
{
	// Yes/no questions and text questions, shown over the panel's window.
	public sealed class WpfDialogs : IDialogs
	{
		public const string Caption = "Battleborn Reborn Control Panel";

		private static Window? Owner => Application.Current?.MainWindow is { IsVisible: true } w ? w : null;

		public bool Confirm(string question)
		{
			Window? owner = Owner;
			MessageBoxResult answer = owner == null
				? MessageBox.Show(question, Caption, MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No)
				: MessageBox.Show(owner, question, Caption, MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No);
			return answer == MessageBoxResult.Yes;
		}

		public string? AskText(string question, string startText)
		{
			var window = new AskTextWindow(question, startText) { Owner = Owner };
			return window.ShowDialog() == true ? window.Text : null;
		}
	}
}
