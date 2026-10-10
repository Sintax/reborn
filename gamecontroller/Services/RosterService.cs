using gamecontroller.Singletons;

namespace gamecontroller.Services
{
	// Finds this machine's public address, then keeps the roster's servers running.
	public class RosterService : BackgroundService
	{
		private readonly ServerRoster _roster;
		private readonly IConfiguration _config;
		private readonly ILogger<RosterService> _logger;

		public RosterService(ServerRoster roster, IConfiguration config, ILogger<RosterService> logger)
		{
			_roster = roster;
			_config = config;
			_logger = logger;
		}

		protected override async Task ExecuteAsync(CancellationToken stoppingToken)
		{
			_roster.PublicIp = _config["PublicIp"];
			while (_roster.PublicIp == null && !stoppingToken.IsCancellationRequested)
			{
				try
				{
					using var http = new HttpClient();
					_roster.PublicIp = (await http.GetStringAsync("https://checkip.amazonaws.com", stoppingToken)).Trim();
					_logger.LogInformation("Public address: {Ip}", _roster.PublicIp);
				}
				catch (Exception e) when (e is not OperationCanceledException)
				{
					_logger.LogWarning("Could not find the public address ({Error}); retrying. Set \"PublicIp\" in appsettings.json to skip this.", e.Message);
					await Task.Delay(10000, stoppingToken);
				}
			}

			while (!stoppingToken.IsCancellationRequested)
			{
				try
				{
					_roster.Reconcile(DateTime.UtcNow);
				}
				catch (Exception e)
				{
					_logger.LogError("Could not start a roster server: {Error}", e.Message);
					await Task.Delay(30000, stoppingToken);
				}
				await Task.Delay(1000, stoppingToken);
			}
		}

		public override Task StopAsync(CancellationToken cancellationToken)
		{
			// The service owns its servers: none should outlive it.
			foreach (var i in _roster.Instances) _roster.MarkFinished(i.MyGuid);
			return base.StopAsync(cancellationToken);
		}
	}
}
