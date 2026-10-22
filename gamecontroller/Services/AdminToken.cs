using System.Security.Cryptography;
using System.Text;

namespace gamecontroller.Services
{
	// The shared secret of the local admin API (Controllers/Admin.cs): admin-token.txt in the
	// matchmaking folder. Made once, the first time the service starts without one, and kept from then
	// on (updates keep it too), so the Control Panel on the same machine can read it. Anyone who can read
	// that folder is already the server's owner. The token itself is never logged or shown.
	public sealed class AdminToken
	{
		public const string FileName = "admin-token.txt";
		public const int Length = 32;

		private readonly byte[] _token;

		private AdminToken(string token) => _token = Encoding.UTF8.GetBytes(token);

		// The token in `path`, or a new one written there when the file is missing or empty.
		public static AdminToken LoadOrCreate(string path)
		{
			string? existing = ReadToken(path);
			if (existing != null) return new AdminToken(existing);

			string fresh = RandomNumberGenerator.GetString(UserListWriter.KeyAlphabet, Length);
			string? dir = Path.GetDirectoryName(Path.GetFullPath(path));
			if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
			string tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				File.WriteAllText(tmp, fresh);
				File.Move(tmp, path, overwrite: File.Exists(path));   // only an empty file is ever replaced
			}
			catch (IOException) when (ReadToken(path) is string other)
			{
				// Another start made one at the same moment: use that.
				fresh = other;
			}
			finally
			{
				try { File.Delete(tmp); } catch { }
			}
			return new AdminToken(fresh);
		}

		private static string? ReadToken(string path)
		{
			if (!File.Exists(path)) return null;
			string text = File.ReadAllText(path).Trim();
			return text.Length > 0 ? text : null;
		}

		// True only for exactly this token. The comparison takes the same time however much of it
		// matches, so a caller cannot guess it a character at a time.
		public bool Matches(string? presented)
		{
			if (string.IsNullOrEmpty(presented)) return false;
			return CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(presented), _token);
		}
	}
}
