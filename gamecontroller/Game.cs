using gamecontroller.Singletons;
using gamecontroller.Models;
using Microsoft.EntityFrameworkCore;
using System.ComponentModel.DataAnnotations;
using System.ComponentModel.DataAnnotations.Schema;
using System.Numerics;

namespace gamecontroller
{
    public class TelemetryContext : DbContext
    {
        public DbSet<TelemetryMatchLogs> Logs { get; set; }

		public string DbPath { get; }

		public TelemetryContext()
		{
			DbPath = "telemetry.db";
		}

		protected override void OnConfiguring(DbContextOptionsBuilder options)
			=> options.UseSqlite($"Data Source={DbPath}");
	}

    public class TelemetryMatchLogs
    {
        [Key]
        public string MatchGUID { get; set; }

        public List<TelemetryEvent> TelemetryEvents { get; set; }
        public List<TelemetrySnapshot> TelemetrySnapshots { get; set; }

        public TelemetryMatchLogs(string matchGUID, TelemetryEvent seedEvent)
        {
            MatchGUID = matchGUID;
            TelemetryEvents = new List<TelemetryEvent>();
            TelemetrySnapshots = new List<TelemetrySnapshot>();
            TelemetryEvents.Add(seedEvent);
        }

		public TelemetryMatchLogs(string matchGUID, TelemetrySnapshot seedSnapshot)
		{
			MatchGUID = matchGUID;
			TelemetryEvents = new List<TelemetryEvent>();
			TelemetrySnapshots = new List<TelemetrySnapshot>();
			TelemetrySnapshots.Add(seedSnapshot);
		}

        public TelemetryMatchLogs()
        {
            MatchGUID = "";
            TelemetryEvents = new List<TelemetryEvent>();
            TelemetrySnapshots = new List<TelemetrySnapshot>();
        }
	}

    public class TelemetryEvent
    {
        [Key]
		public DateTime Timestamp { get; set; }

		public required string MatchGUID { get; set; }

		public required string EventName { get; set; }

        public bool IsError { get; set; }
    }

    public class TelemetrySnapshot
    {
        [Key]
        public DateTime Timestamp { get; set; }

		public required string MatchGUID { get; set; }

		public float TargetTickrate { get; set; }

        public float EffectiveTickrate { get; set; }

        public required List<int> ReplicationFNames { get; set; }

        public int PlayersConnected { get; set; }

        public int MaxPlayers { get; set; }

        public int MemoryUsageMB { get; set; }
    }

    public class GameInstance
    {
        public string MyGuid;   // private: the server's token (X-Server-Token); never published
        public string PublicId { get; set; } = Guid.NewGuid().ToString();   // what the server browser shows
        public int PID { get; set; }
        public string ConnectionString { get; set; }
        public int Port { get; set; }
        public GameCreationConfig Config { get; set; }
        public DateTime LastServerCheckIn { get; set; }
        public DateTime CreationTime { get; set; }

        // The standing roster slot this server fills (ServerRoster.cs); null for a game a player started.
        public RosterSlot? Slot { get; set; }
        public int CurrentNumPlayers { get; set; }
        public bool MatchStarted { get; set; }
        public bool HasCheckedIn { get; set; }
        public bool Finished { get; set; }

        // On-request games (ServerRoster.StartRequested): who started it and when it was last busy.
        public string? KeyLabel { get; set; }
        public DateTime? FirstCheckIn { get; set; }
        public DateTime? LastNonEmpty { get; set; }
        public bool EverHadPlayers { get; set; }

        public GameInstance()
        {
            MyGuid = Guid.NewGuid().ToString();
            CreationTime = DateTime.UtcNow;
            LastServerCheckIn = DateTime.UtcNow;
        }
    }

    public class GameCreationConfig
    {
        public string InstanceName { get; set; }

		public string HumanReadableInstanceMapMode { get; set; }

		public string ServerStartupCommand { get; set; }

		public int MaxNumPlayers { get; set; }

        // UDP port the server should listen on (Init.cpp reads "port").
        public int Port { get; set; } = 7777;

        // Every player gets a hero picked by the timer, whatever they choose (Init.cpp reads "randomHero").
        public bool RandomHero { get; set; }

        // The game's draft-style value, e.g. "BanPick5v5"; empty means Quick Select (Init.cpp reads "draftStyle").
        public string DraftStyle { get; set; } = "";

        public GameCreationConfig(string instanceName, string mapMode, string serverStartupCommand, int maxNumPlayers)
        {
            InstanceName = instanceName;
            HumanReadableInstanceMapMode = mapMode;
            ServerStartupCommand = serverStartupCommand;
            MaxNumPlayers = maxNumPlayers;
        }
    }
}
