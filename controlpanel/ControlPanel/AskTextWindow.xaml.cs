using System.Windows;

namespace ControlPanel
{
	// A question with a text box (used for Rename and Confirm name).
	public partial class AskTextWindow : Window
	{
		public AskTextWindow(string question, string startText)
		{
			InitializeComponent();
			Question.Text = question;
			Answer.Text = startText;
			Loaded += (_, _) =>
			{
				Answer.Focus();
				Answer.SelectAll();
			};
		}

		public string Text => Answer.Text;

		private void Ok_Click(object sender, RoutedEventArgs e) => DialogResult = true;
	}
}
