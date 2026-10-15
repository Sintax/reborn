namespace gamecontroller.Services
{
	// Who may start games on this server: appsettings.json "StartKeys": { "label": "key" }.
	// Each friend holds one key in Win64\reborn-key.txt; the label is only for the log.
	public class StartKeys
	{
		private readonly Dictionary<string, string> _labelByKey = new(StringComparer.Ordinal);

		public StartKeys(IConfiguration config)
		{
			foreach (var child in config.GetSection("StartKeys").GetChildren())
			{
				if (!string.IsNullOrEmpty(child.Value)) _labelByKey[child.Value] = child.Key;
			}
		}

		public int Count => _labelByKey.Count;

		public string? LabelFor(string? key)
		{
			if (string.IsNullOrEmpty(key)) return null;
			return _labelByKey.TryGetValue(key, out string? label) ? label : null;
		}
	}
}
