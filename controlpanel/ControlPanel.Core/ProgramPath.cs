namespace ControlPanel.Core
{
	// Finds a program ("gh") the way the command prompt does, but only in PATH's absolute folders.
	// Not found means null: the caller must not hand a bare name to Windows, whose own search looks in
	// the panel's folder first.
	public static class ProgramPath
	{
		// file: a bare name ("gh", tried with each PATHEXT ending), a name with its ending ("gh.exe"),
		// or a full path (used if it exists). A relative path with folders in it is never used.
		public static string? Find(string file, string pathVariable, string pathExt)
		{
			if (string.IsNullOrWhiteSpace(file)) return null;
			try
			{
				if (Path.IsPathRooted(file)) return Path.IsPathFullyQualified(file) && File.Exists(file) ? file : null;
				if (file.Contains('\\') || file.Contains('/')) return null;

				string[] endings = Path.HasExtension(file)
					? new[] { "" }
					: (pathExt ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
				foreach (string entry in (pathVariable ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
				{
					string dir = entry.Trim('"');
					if (!Path.IsPathFullyQualified(dir)) continue;   // "." or "bin" would depend on the current folder
					foreach (string ending in endings)
					{
						string candidate = Path.Combine(dir, file + ending);
						if (File.Exists(candidate)) return candidate;
					}
				}
			}
			catch (ArgumentException) { }   // characters a path can't have
			return null;
		}
	}
}
