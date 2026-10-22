using System.ComponentModel;
using System.Diagnostics;
using ControlPanel.Core;

namespace ControlPanel
{
	// Starts, stops and checks programs for the view-models. Never throws (IProcessRunner's rule).
	public sealed class WindowsProcessRunner : IProcessRunner
	{
		private const int UacRefused = 1223;   // ERROR_CANCELLED: the owner answered No to Windows' question

		// The program could not be run for another reason (not a refusal). Not 0 and not -1, so the
		// panel says the update failed rather than "answer Yes next time".
		public const int CouldNotRun = -2;

		public bool Start(string exe, string args, string workingDir)
		{
			try
			{
				using Process? p = Process.Start(new ProcessStartInfo
				{
					FileName = exe,
					Arguments = args,
					WorkingDirectory = workingDir,
					UseShellExecute = true,
					WindowStyle = ProcessWindowStyle.Minimized,
				});
				return p != null;
			}
			catch (Exception)
			{
				return false;
			}
		}

		public Task<int> RunElevated(string exe, string args) => Task.Run(() =>
		{
			try
			{
				// Arguments go through as one raw string: cmd's own quoting must reach it unchanged.
				using Process? p = Process.Start(new ProcessStartInfo
				{
					FileName = exe,
					Arguments = args,
					UseShellExecute = true,
					Verb = "runas",
					WindowStyle = ProcessWindowStyle.Hidden,
				});
				if (p == null) return CouldNotRun;
				p.WaitForExit();
				return p.ExitCode;
			}
			catch (Win32Exception e) when (e.NativeErrorCode == UacRefused)
			{
				return -1;
			}
			catch (Exception)
			{
				return CouldNotRun;
			}
		});

		public bool IsRunning(string processName)
		{
			try
			{
				Process[] found = Process.GetProcessesByName(processName);
				foreach (Process p in found) p.Dispose();
				return found.Length > 0;
			}
			catch (Exception)
			{
				return false;
			}
		}

		public void StopAll(string processName)
		{
			Process[] found;
			try { found = Process.GetProcessesByName(processName); }
			catch (Exception) { return; }
			foreach (Process p in found)
			{
				try { p.Kill(); }
				catch (Exception) { }   // already gone, or not ours to stop
				finally { p.Dispose(); }
			}
		}

		public void OpenFolder(string path)
		{
			try
			{
				using Process? p = Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
			}
			catch (Exception) { }
		}
	}
}
