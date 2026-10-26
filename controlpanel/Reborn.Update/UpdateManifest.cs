using System.Text.Json;

namespace Reborn.Update;

/// <summary>A release's update.json, written and signed by tools/release.py:
/// {"version","build","notes","commit","zip","sha256","size"}.</summary>
public sealed record UpdateManifest(string Version, int Build, string Notes, string Commit, string Zip, string Sha256, long Size)
{
    /// <summary>The manifest, or null when any field is missing, has the wrong type or does not make sense
    /// (build not matching the version, a zip name with a folder in it, a bad hash, an empty zip).
    /// Check the signature over the exact text before trusting it.</summary>
    public static UpdateManifest? Parse(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;

            string? version = Text(root, "version");
            string? notes = Text(root, "notes");
            string? commit = Text(root, "commit");
            string? zip = Text(root, "zip");
            string? sha256 = Text(root, "sha256");
            if (version is null || notes is null || commit is null || zip is null || sha256 is null) return null;
            if (!root.TryGetProperty("build", out JsonElement b) || b.ValueKind != JsonValueKind.Number || !b.TryGetInt32(out int build)) return null;
            if (!root.TryGetProperty("size", out JsonElement s) || s.ValueKind != JsonValueKind.Number || !s.TryGetInt64(out long size)) return null;

            if (build < 1 || Versions.Parse(version) != build) return null;
            if (commit.Length == 0 || size <= 0) return null;
            if (!IsPlainFileName(zip)) return null;
            if (sha256.Length != 64 || !sha256.All(Uri.IsHexDigit)) return null;

            return new UpdateManifest(version, build, notes, commit, zip, sha256.ToLowerInvariant(), size);
        }
        catch (Exception e) when (e is JsonException or ArgumentException)
        {
            return null;
        }
    }

    static string? Text(JsonElement root, string name) =>
        root.TryGetProperty(name, out JsonElement e) && e.ValueKind == JsonValueKind.String ? e.GetString() : null;

    static bool IsPlainFileName(string name) =>
        name.Length > 0
        && name != "." && name != ".."
        && name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0
        && name.IndexOfAny(new[] { '/', '\\', ':' }) < 0;
}
