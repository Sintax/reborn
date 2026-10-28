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
		// Starts a program (minimized unless asked otherwise, in workingDir) without waiting. False if it could not start.
		bool Start(string exe, string args, string workingDir, bool minimized = true);

		// Runs a program with no window and waits for it. Its exit code, or -1 if it could not start.
		// output: everything it printed (standard output, then errors). Call it off the UI thread.
		int Run(string file, string args, out string output);

		// The same, with extra environment variables for that program only (never on its command line).
		int Run(string file, string args, IReadOnlyDictionary<string, string> environment, out string output);

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

	// The panel's own program, for handing over to the updater (which waits for it to close).
	public interface IPanelApp
	{
		int ProcessId { get; }

		// The full path of the panel's exe, which the updater starts again when it is done.
		string ExePath { get; }

		// Closes the panel.
		void Close();
	}

	public interface IDialogs
	{
		// A yes/no question. True for yes.
		bool Confirm(string question);

		// A question with a text box, filled with startText. The text typed, or null for Cancel.
		string? AskText(string question, string startText);
	}
}
