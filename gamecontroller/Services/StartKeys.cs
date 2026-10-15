using Microsoft.Extensions.Primitives;

namespace gamecontroller.Services
{
	// Who may start games on this server: appsettings.json "StartKeys": { "label": "key" }.
	// Each friend holds one key in Win64\reborn-key.txt; the label is only for the log.
	// The list is rebuilt whenever the settings file changes, so adding or removing a friend
	// takes effect without restarting the service (and without ending running games).
	public class StartKeys
	{
		private readonly IConfiguration _config;
		private readonly ILogger<StartKeys>? _logger;
		// Replaced as a whole on each reload; readers always see one complete list.
		private volatile Dictionary<string, string> _labelByKey;

		public StartKeys(IConfiguration config, ILogger<StartKeys>? logger = null)
		{
			_config = config;
			_logger = logger;
			_labelByKey = Read();
			ChangeToken.OnChange(config.GetReloadToken, () =>
			{
				var fresh = Read();
				_labelByKey = fresh;
				// Labels only: the keys themselves never go into the log.
				_logger?.LogInformation("Start keys reloaded: {Count} ({Labels})", fresh.Count, string.Join(", ", fresh.Values.OrderBy(v => v)));
			});
		}

		private Dictionary<string, string> Read()
		{
			var map = new Dictionary<string, string>(StringComparer.Ordinal);
			foreach (var child in _config.GetSection("StartKeys").GetChildren())
			{
				if (!string.IsNullOrEmpty(child.Value)) map[child.Value] = child.Key;
			}
			return map;
		}

		public int Count => _labelByKey.Count;

		public string? LabelFor(string? key)
		{
			if (string.IsNullOrEmpty(key)) return null;
			return _labelByKey.TryGetValue(key, out string? label) ? label : null;
		}
	}
}
