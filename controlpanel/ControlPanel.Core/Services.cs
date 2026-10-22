namespace ControlPanel.Core
{
	// The few things the window project does for the view-models with Windows itself. Swapped for
	// fakes in the tests. Calls come from the UI thread.

	public interface IClipboard
	{
		void SetText(string text);
	}

	// Every method must never throw: report failure through the return value (false, -1) instead.
	// (The panel also catches anything that slips through and shows it as a plain message.)
	public interface IProcessRunner
	{
		// Starts a program (minimized, in workingDir) without waiting. False if it could not start.
		bool Start(string exe, string args, string workingDir);

		// Runs a program hidden, as administrator (Windows asks the owner), and waits. Its exit code,
		// -1 when the owner said no, or another non-zero code when it could not run.
		Task<int> RunElevated(string exe, string args);

		// Whether a process by this name (no ".exe") is running.
		bool IsRunning(string processName);

		// Stops every process by this name (no ".exe"), as taskkill /f does.
		void StopAll(string processName);

		// Opens a folder in File Explorer (does nothing if it can't).
		void OpenFolder(string path);
	}

	public interface IDialogs
	{
		// A yes/no question. True for yes.
		bool Confirm(string question);

		// A question with a text box, filled with startText. The text typed, or null for Cancel.
		string? AskText(string question, string startText);
	}
}
