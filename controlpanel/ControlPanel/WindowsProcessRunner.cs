using System.ComponentModel;
using System.Diagnostics;
using System.IO;
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

		public bool Start(string exe, string args, string workingDir, bool minimized = true)
		{
			try
			{
				// With shell execute, null does not mean failure: Windows may hand the start to a process
				// that is already running and give no handle back. A real failure throws instead.
				using Process? p = Process.Start(new ProcessStartInfo
				{
					FileName = exe,
					WorkingDirectory = workingDir,
					UseShellExecute = true,
					WindowStyle = minimized ? ProcessWindowStyle.Minimized : ProcessWindowStyle.Normal,
				});
				return true;
			}
			catch (Exception)
			{
				return false;
			}
		}

		// A program that never ends (gh stuck on a dead connection) is stopped after this long.
		public static readonly TimeSpan RunLimit = TimeSpan.FromMinutes(30);

		public int Run(string file, string args, out string output) => Run(file, args, new Dictionary<string, string>(), out output);

		public int Run(string file, string args, IReadOnlyDictionary<string, string> environment, out string output) =>
			RunCore(file, info => info.Arguments = args, environment, RunLimit, out output);

		public int Run(string file, IReadOnlyList<string> arguments, TimeSpan limit, out string output) =>
			RunCore(file, info => { foreach (string a in arguments) info.ArgumentList.Add(a); }, new Dictionary<string, string>(), limit, out output);

		private static int RunCore(string file, Action<ProcessStartInfo> arguments, IReadOnlyDictionary<string, string> environment, TimeSpan limit, out string output)
		{
			output = "";
			// Only PATH's absolute folders: never Windows' own search, which looks next to the panel first.
			string? program = ProgramPath.Find(file, Environment.GetEnvironmentVariable("PATH") ?? "",
				Environment.GetEnvironmentVariable("PATHEXT") ?? ".COM;.EXE;.BAT;.CMD");
			if (program == null)
			{
				output = file + ProgramPath.NotFoundSuffix;
				return -1;
			}
			Process? p = null;
			try
			{
				var info = new ProcessStartInfo
				{
					FileName = program,
					UseShellExecute = false,
					CreateNoWindow = true,
					RedirectStandardInput = true,    // closed at once: nothing can sit waiting for an answer
					RedirectStandardOutput = true,
					RedirectStandardError = true,
					StandardOutputEncoding = System.Text.Encoding.UTF8,
					StandardErrorEncoding = System.Text.Encoding.UTF8,
				};
				arguments(info);
				foreach (var pair in environment) info.Environment[pair.Key] = pair.Value;
				p = Process.Start(info);
				if (p == null) return -1;
			}
			catch (Exception)
			{
				p?.Dispose();
				return -1;
			}

			using (p)
			{
				try
				{
					p.StandardInput.Close();
					// Both streams are read at the same time, so a full one can't stall the other.
					Task<string> stdout = p.StandardOutput.ReadToEndAsync();
					Task<string> stderr = p.StandardError.ReadToEndAsync();
					if (!p.WaitForExit(limit))
					{
						try { p.Kill(entireProcessTree: true); } catch (Exception) { }
						output = "It took longer than " + (int)limit.TotalMinutes + " minutes and was stopped.";
						return -1;
					}
					p.WaitForExit();
					output = stdout.Result + stderr.Result;
					return p.ExitCode;
				}
				catch (Exception e)
				{
					output = e.Message;
					return -1;
				}
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
