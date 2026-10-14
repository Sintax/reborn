namespace gamecontroller.Services
{
	// Writes the service's log to matchmaking.log next to gamecontroller.exe as well as the console,
	// so someone remoting in (or Claude on that machine) can read it without the console window.
	// The previous run's log is kept as matchmaking.previous.log.
	public sealed class FileLoggerProvider : ILoggerProvider
	{
		private readonly StreamWriter _writer;
		private readonly object _lock = new();

		public FileLoggerProvider(string path)
		{
			string previous = Path.ChangeExtension(path, ".previous.log");
			if (File.Exists(path)) File.Move(path, previous, overwrite: true);
			var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
			_writer = new StreamWriter(stream) { AutoFlush = true };
		}

		public ILogger CreateLogger(string categoryName) => new FileLogger(this, categoryName);

		internal void Write(string line)
		{
			lock (_lock) _writer.WriteLine(line);
		}

		public void Dispose()
		{
			lock (_lock) _writer.Dispose();
		}

		private sealed class FileLogger : ILogger
		{
			private readonly FileLoggerProvider _provider;
			private readonly string _category;

			public FileLogger(FileLoggerProvider provider, string category)
			{
				_provider = provider;
				_category = category;
			}

			public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

			public bool IsEnabled(LogLevel logLevel) => logLevel != LogLevel.None;

			public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
			{
				string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {logLevel,-11} {_category}: {formatter(state, exception)}";
				if (exception != null) line += Environment.NewLine + exception;
				_provider.Write(line);
			}
		}
	}
}
