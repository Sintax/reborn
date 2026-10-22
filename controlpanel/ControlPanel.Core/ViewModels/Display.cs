using System.Globalization;

namespace ControlPanel.Core.ViewModels
{
	// Times and dates as the owner reads them.
	public static class Display
	{
		public static DateTime Utc(DateTime t) => t.Kind switch
		{
			DateTimeKind.Utc => t,
			DateTimeKind.Local => t.ToUniversalTime(),
			_ => DateTime.SpecifyKind(t, DateTimeKind.Utc),
		};

		// "2026-10-22" in the owner's time zone, or "" when there is no date.
		public static string Date(DateTime? utc, TimeZoneInfo zone) =>
			utc == null ? "" : TimeZoneInfo.ConvertTimeFromUtc(Utc(utc.Value), zone).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);

		// "12 min", "2 h 5 min", "under a minute".
		public static string Duration(TimeSpan span)
		{
			if (span < TimeSpan.FromMinutes(1)) return "under a minute";
			if (span < TimeSpan.FromHours(1)) return $"{(int)span.TotalMinutes} min";
			return $"{(int)span.TotalHours} h {span.Minutes} min";
		}

		// "never", "just now", "5 min ago", "3 h ago", or the date for older times.
		public static string Ago(DateTime? utc, DateTime nowUtc, TimeZoneInfo zone)
		{
			if (utc == null) return "never";
			TimeSpan span = nowUtc - Utc(utc.Value);
			if (span < TimeSpan.FromMinutes(1)) return "just now";
			if (span < TimeSpan.FromHours(1)) return $"{(int)span.TotalMinutes} min ago";
			if (span < TimeSpan.FromHours(24)) return $"{(int)span.TotalHours} h ago";
			return Date(utc, zone);
		}
	}
}
