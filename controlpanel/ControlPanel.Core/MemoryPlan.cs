using System.Globalization;
using System.Runtime.InteropServices;

namespace ControlPanel.Core
{
	// How much memory the game servers need against what this machine has. Each game server takes
	// about 1.3 GB; about 1 GB is left for Windows and matchmaking.
	public sealed class MemoryPlan
	{
		public const double GbPerGame = 1.3;
		public const double GbForWindows = 1.0;

		public double TotalGb { get; }
		public double FreeGb { get; }
		public int Games { get; }

		public MemoryPlan(double totalGb, double freeGb, int ports)
		{
			TotalGb = totalGb;
			FreeGb = freeGb;
			Games = ports;
		}

		// Rounded to 0.1 GB so 3 x 1.3 is 3.9, not 3.9000000000000004.
		public double NeededGb => Math.Round(Games * GbPerGame, 2);

		public bool TooMany => NeededGb > Math.Round(TotalGb - GbForWindows, 2);

		public string Summary => string.Format(CultureInfo.InvariantCulture,
			"Each game uses about {0:0.0} GB. {1} {2} about {3:0.0} GB; this server has {4:0.0} GB ({5:0.0} GB free).",
			GbPerGame, Games == 1 ? "1 game" : Games + " games", Games == 1 ? "needs" : "need", NeededGb, TotalGb, FreeGb);

		public string Warning => TooMany
			? "That is more games than this server's memory can hold. Games may crash or run slowly; remove a port or add memory."
			: "";
	}

	public sealed record MemoryReading(double TotalGb, double FreeGb);

	// Reads this machine's memory. Swapped for a fake in tests.
	public interface ISystemMemory
	{
		MemoryReading? Read();   // null when it can't be read
	}

	public sealed class WindowsMemory : ISystemMemory
	{
		[StructLayout(LayoutKind.Sequential)]
		private struct MemoryStatusEx
		{
			public uint Length;
			public uint MemoryLoad;
			public ulong TotalPhys;
			public ulong AvailPhys;
			public ulong TotalPageFile;
			public ulong AvailPageFile;
			public ulong TotalVirtual;
			public ulong AvailVirtual;
			public ulong AvailExtendedVirtual;
		}

		[DllImport("kernel32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);

		public MemoryReading? Read()
		{
			if (!OperatingSystem.IsWindows()) return null;
			var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
			if (!GlobalMemoryStatusEx(ref status)) return null;
			const double gb = 1024.0 * 1024 * 1024;
			return new MemoryReading(status.TotalPhys / gb, status.AvailPhys / gb);
		}
	}
}
