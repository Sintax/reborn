using System.Security.Cryptography;

namespace ControlPanel.Core
{
	// Player names and join keys. The rules are the server's: tools/vm.py valid_name / valid_key and
	// gamecontroller/Services/UserListWriter.cs (UserList.CleanName for names).
	public static class Keys
	{
		public const string Alphabet = "ABCDEFGHJKMNPQRSTVWXYZ23456789";   // no 0/O/1/I/L/U: friends may type it
		public const int Length = 24;
		public const int MaxNameLength = 24;

		// A fresh random key from the crypto random source.
		public static string New() => RandomNumberGenerator.GetString(Alphabet, Length);

		// 1 to 24 of A-Z, a-z, 0-9, _ and -.
		public static bool ValidName(string? name)
		{
			if (string.IsNullOrEmpty(name) || name.Length > MaxNameLength) return false;
			foreach (char c in name)
				if (!(c is (>= 'A' and <= 'Z') or (>= 'a' and <= 'z') or (>= '0' and <= '9') or '_' or '-')) return false;
			return true;
		}

		public static bool ValidKey(string? key) =>
			key != null && key.Length == Length && key.All(c => Alphabet.Contains(c));

		public const string BadName = "That name can't be used. Use 1 to 24 letters, numbers, _ or -.";
	}
}
