using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace gamecontroller.Services
{
	// Saves the game ports in the service's appsettings.json for the next start (Program.cs reads
	// "GamePorts" once, at startup; running games keep their ports). Only the GamePorts value is
	// replaced: every other setting, comment and line stays exactly as it was. The new file is
	// checked, written to a .tmp beside it and renamed over the old one, so the settings reload never
	// sees half a file. A settings file that cannot be read is left alone.
	public sealed class SettingsWriter
	{
		public const int MaxPorts = 16;
		public const int LowestPort = 1024;
		public const int HighestPort = 65535;
		public static readonly IReadOnlyList<int> DefaultPorts = new[] { 7777, 7778, 7779 };   // as Program.cs

		public const string Damaged = "The settings file (appsettings.json) could not be read; fix or restore it first. Nothing was changed.";
		public const string NotSaved = "The settings file could not be saved, so nothing was changed. Try again.";

		// The same leniency as the service's own settings loader: comments and trailing commas are fine.
		private static readonly JsonDocumentOptions Lenient = new() { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };
		private static readonly object WriteLock = new();

		public string Path { get; }

		public SettingsWriter(string path) => Path = path;

		// Plain-language reason a port list cannot be used, or null when it is fine.
		public static string? CheckPorts(IReadOnlyList<int>? ports)
		{
			if (ports == null || ports.Count == 0 || ports.Count > MaxPorts) return $"Pick 1 to {MaxPorts} game ports.";
			if (ports.Any(p => p < LowestPort || p > HighestPort)) return $"Game ports must be numbers from {LowestPort} to {HighestPort}.";
			if (ports.Distinct().Count() != ports.Count) return "Each game port can only be listed once.";
			return null;
		}

		// The ports saved in the file now (what the next start will use); the defaults when the file
		// names none; null when the file cannot be read.
		public IReadOnlyList<int>? ReadPorts()
		{
			try
			{
				JsonNode? root = JsonNode.Parse(File.ReadAllText(Path), documentOptions: Lenient);
				if (root is not JsonObject obj) return null;
				JsonNode? ports = obj.FirstOrDefault(p => string.Equals(p.Key, "GamePorts", StringComparison.OrdinalIgnoreCase)).Value;
				if (ports is not JsonArray list) return DefaultPorts;
				var result = new List<int>();
				foreach (JsonNode? p in list)
				{
					if (p is JsonValue v && v.TryGetValue(out int n)) result.Add(n);
					else if (p is JsonValue s && s.TryGetValue(out string? text) && int.TryParse(text, out n)) result.Add(n);
				}
				return result;
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException)
			{
				return null;
			}
		}

		public AdminRefusal? SavePorts(IReadOnlyList<int>? ports)
		{
			string? problem = CheckPorts(ports);
			if (problem != null) return new AdminRefusal(400, problem);
			string value = "[ " + string.Join(", ", ports!) + " ]";

			lock (WriteLock)
			{
				string text;
				try { text = File.ReadAllText(Path); }   // drops a UTF-8 BOM; it is written back below
				catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new AdminRefusal(409, Damaged); }
				bool bom = HasBom();

				string? updated = Replaced(text, value);
				if (updated == null) return new AdminRefusal(409, Damaged);

				// The result must still read as settings, with exactly the new ports.
				try
				{
					if (JsonNode.Parse(updated, documentOptions: Lenient) is not JsonObject check
						|| check.FirstOrDefault(p => string.Equals(p.Key, "GamePorts", StringComparison.OrdinalIgnoreCase)).Value is not JsonArray saved
						|| !saved.Select(p => p!.GetValue<int>()).SequenceEqual(ports!))
						return new AdminRefusal(409, Damaged);
				}
				catch (Exception e) when (e is JsonException or ArgumentException or InvalidOperationException or FormatException)
				{
					return new AdminRefusal(409, Damaged);
				}

				return Save(updated, bom) ? null : new AdminRefusal(500, NotSaved);
			}
		}

		// `text` with the top-level GamePorts value swapped for `value`, or with a GamePorts line added
		// when there is none; null when the text is not a settings object.
		private static string? Replaced(string text, string value)
		{
			byte[] utf8 = Encoding.UTF8.GetBytes(text);
			var reader = new Utf8JsonReader(utf8, new JsonReaderOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
			try
			{
				if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return null;
				int lastValueEnd = -1;
				while (reader.Read())
				{
					if (reader.TokenType == JsonTokenType.EndObject)
					{
						// No GamePorts: add one after the last setting (or as the only one).
						int at = lastValueEnd < 0 ? (int)reader.TokenStartIndex : lastValueEnd;
						string head = Encoding.UTF8.GetString(utf8, 0, at);
						string tail = Encoding.UTF8.GetString(utf8, at, utf8.Length - at);
						string nl = text.Contains("\r\n") ? "\r\n" : "\n";
						string line = (lastValueEnd < 0 ? nl : "," + nl) + "  \"GamePorts\": " + value;
						return head + line + (lastValueEnd < 0 ? nl : "") + tail;
					}
					// reader is on a top-level property name
					bool isPorts = string.Equals(reader.GetString(), "GamePorts", StringComparison.OrdinalIgnoreCase);
					if (!reader.Read()) return null;
					int start = (int)reader.TokenStartIndex;
					reader.Skip();
					int end = (int)reader.BytesConsumed;
					if (isPorts)
						return Encoding.UTF8.GetString(utf8, 0, start) + value + Encoding.UTF8.GetString(utf8, end, utf8.Length - end);
					lastValueEnd = end;
				}
				return null;
			}
			catch (JsonException)
			{
				return null;
			}
		}

		private bool HasBom()
		{
			try
			{
				using var f = File.OpenRead(Path);
				Span<byte> head = stackalloc byte[3];
				return f.Read(head) == 3 && head.SequenceEqual(Encoding.UTF8.Preamble);
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return false;
			}
		}

		private bool Save(string text, bool bom)
		{
			string tmp = Path + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				File.WriteAllText(tmp, text, new UTF8Encoding(bom));
				for (int attempt = 1; ; attempt++)
				{
					try
					{
						File.Move(tmp, Path, overwrite: true);
						return true;
					}
					catch (Exception e) when (attempt < 5 && e is IOException or UnauthorizedAccessException)
					{
						Thread.Sleep(50);   // the settings reload or a virus scanner had it open for a moment
					}
				}
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException)
			{
				return false;
			}
			finally
			{
				try { File.Delete(tmp); } catch { }
			}
		}
	}
}
