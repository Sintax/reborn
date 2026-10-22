using ControlPanel.Core;

namespace ControlPanel
{
	// The Windows clipboard. SetText can throw when another program holds the clipboard; the
	// view-model catches that and asks the owner to try again.
	public sealed class WindowsClipboard : IClipboard
	{
		public void SetText(string text) => System.Windows.Clipboard.SetText(text);
	}
}
