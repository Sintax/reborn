using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Mvc;

namespace gamecontroller.Controllers
{
	// Serves the signed player update (the two mod files) so friends' games can download it.
	// Open to everyone on purpose: the files are signed and the game checks the signature itself.
	// So the one job here is to hand out nothing but the two named files from the one update folder:
	// the name a caller sends is only ever compared against the list below, never turned into a path.
	[ApiController]
	[Route("api/updates")]
	public class Updates : ControllerBase
	{
		public const string ManifestFile = "manifest.json";
		public const string SignatureFile = "manifest.json.sig";
		public const string FolderKey = "PlayerUpdateDir";
		public const string FolderName = "player-update";

		// The only files that may ever be sent. Exact, case-sensitive match.
		private static readonly string[] Served = { "reborn.dll", "dxgi.dll" };

		private readonly IConfiguration _config;

		public Updates(IConfiguration config) => _config = config;

		public static string Folder(IConfiguration config)
		{
			string? set = config[FolderKey];
			return string.IsNullOrWhiteSpace(set) ? Path.Combine(AppContext.BaseDirectory, FolderName) : set;
		}

		private NotFoundObjectResult NoUpdate() => NotFound(new { message = "No player update on this server." });

		// The manifest and its signature, exactly as stored: the game verifies the signature over these
		// very characters, so nothing is parsed or re-written here. Null when either is missing.
		private (string Manifest, string Signature)? ReadSigned()
		{
			string dir = Folder(_config);
			try
			{
				string manifest = Path.Combine(dir, ManifestFile), signature = Path.Combine(dir, SignatureFile);
				if (!System.IO.File.Exists(manifest) || !System.IO.File.Exists(signature)) return null;
				return (Text(System.IO.File.ReadAllBytes(manifest)), Text(System.IO.File.ReadAllBytes(signature)));
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
		}

		// The bytes as text without touching them: a byte-order mark stays in the text (GetString keeps it)
		// and nothing is guessed from it, so what goes out encodes back to exactly what was stored.
		private static string Text(byte[] bytes) => new UTF8Encoding(false).GetString(bytes);

		// Updates change, and a stale copy could hide a newer one (or keep an old signed one around).
		private void NoStore() => Response.Headers.CacheControl = "no-store";

		// The file names the manifest lists. A manifest that cannot be read as JSON lists nothing.
		private static HashSet<string> Listed(string manifest)
		{
			var names = new HashSet<string>(StringComparer.Ordinal);
			try
			{
				using var doc = JsonDocument.Parse(manifest);
				if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("files", out var files)
					|| files.ValueKind != JsonValueKind.Array) return names;
				foreach (var f in files.EnumerateArray())
					if (f.ValueKind == JsonValueKind.Object && f.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String)
						names.Add(n.GetString()!);
			}
			catch (JsonException) { }
			return names;
		}

		[HttpGet("player")]
		public IActionResult GetPlayer()
		{
			NoStore();
			var signed = ReadSigned();
			if (signed == null) return NoUpdate();
			return Ok(new { manifest = signed.Value.Manifest, signature = signed.Value.Signature });
		}

		[HttpGet("player/files/{name}")]
		public IActionResult GetPlayerFile(string name)
		{
			NoStore();
			// Match against the list and carry on with the list's own spelling, never the caller's.
			string? file = Served.FirstOrDefault(s => string.Equals(s, name, StringComparison.Ordinal));
			if (file == null) return NoUpdate();

			var signed = ReadSigned();
			if (signed == null || !Listed(signed.Value.Manifest).Contains(file)) return NoUpdate();

			try
			{
				// Streamed from disk, not read into memory first (each file can be tens of MB).
				string path = Path.GetFullPath(Path.Combine(Folder(_config), file));
				if (!System.IO.File.Exists(path)) return NoUpdate();
				return PhysicalFile(path, "application/octet-stream");
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return NoUpdate(); }
		}
	}
}
