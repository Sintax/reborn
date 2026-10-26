using System.Text.Json;

namespace Reborn.Update;

/// <summary>Reads matchmaking's /api/games answer.</summary>
public static class GamesList
{
    /// <summary>True when the list holds at least one game whose "ready" is true. Matchmaking lists a game
    /// the moment it launches the game server, with "ready": false until that server has started and
    /// checked in, so only a ready game shows the new version really works. The name is matched in any
    /// letter case. Never throws.</summary>
    public static bool HasReadyGame(string? json)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(json)) return false;
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return false;
            foreach (JsonElement game in doc.RootElement.EnumerateArray())
            {
                if (game.ValueKind != JsonValueKind.Object) continue;
                foreach (JsonProperty prop in game.EnumerateObject())
                    if (string.Equals(prop.Name, "ready", StringComparison.OrdinalIgnoreCase) && prop.Value.ValueKind == JsonValueKind.True)
                        return true;
            }
            return false;
        }
        catch (Exception) { return false; }
    }
}
