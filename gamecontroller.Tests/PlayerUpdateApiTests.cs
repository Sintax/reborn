using System.Text.Json;
using gamecontroller.Controllers;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;

namespace gamecontroller.Tests;

public class PlayerUpdateApiTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "gcupdate-" + Guid.NewGuid().ToString("N"));
    string UpdateDir => Path.Combine(_root, "player-update");

    public PlayerUpdateApiTests() => Directory.CreateDirectory(_root);
    public void Dispose() { try { Directory.Delete(_root, true); } catch { } }

    // Deliberately odd spacing: the client verifies the signature over these exact characters.
    const string Manifest = "{ \"version\":\"1.0.7\",  \"build\":7,\"notes\":\"Fixes the lobby.\",\n \"files\":[{\"name\":\"reborn.dll\",\"sha256\":\"aa\",\"size\":4},{\"name\":\"dxgi.dll\",\"sha256\":\"bb\",\"size\":3}] }\n";
    const string Signature = "c2lnbmF0dXJlLWJhc2U2NA==";
    static readonly byte[] RebornDll = { 1, 2, 3, 4 };
    static readonly byte[] DxgiDll = { 9, 8, 7 };

    Updates Make(bool createFolder = true, string? manifest = Manifest, string? signature = Signature, bool rebornDll = true, bool dxgiDll = true)
    {
        if (createFolder)
        {
            Directory.CreateDirectory(UpdateDir);
            if (manifest != null) File.WriteAllText(Path.Combine(UpdateDir, "manifest.json"), manifest);
            if (signature != null) File.WriteAllText(Path.Combine(UpdateDir, "manifest.json.sig"), signature);
            if (rebornDll) File.WriteAllBytes(Path.Combine(UpdateDir, "reborn.dll"), RebornDll);
            if (dxgiDll) File.WriteAllBytes(Path.Combine(UpdateDir, "dxgi.dll"), DxgiDll);
        }
        // Things next to the folder that must never be reachable.
        File.WriteAllText(Path.Combine(_root, "admin-token.txt"), "SECRET-TOKEN");
        File.WriteAllText(Path.Combine(_root, "appsettings.json"), "{ \"secret\": true }");
        if (createFolder) File.WriteAllText(Path.Combine(UpdateDir, "reborn.pdb"), "symbols");
        var config = new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["PlayerUpdateDir"] = UpdateDir }).Build();
        return new Updates(config) { ControllerContext = new ControllerContext { HttpContext = new DefaultHttpContext() } };
    }

    static string Message(IActionResult result)
    {
        var notFound = Assert.IsType<NotFoundObjectResult>(result);
        return JsonSerializer.Serialize(notFound.Value);
    }

    [Fact]
    public void Missing_folder_gives_404_with_a_plain_message()
    {
        var c = Make(createFolder: false);
        Assert.Contains("No player update on this server.", Message(c.GetPlayer()));
        Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile("reborn.dll"));
    }

    [Fact]
    public void Missing_manifest_or_signature_gives_404()
    {
        var c = Make();
        File.Delete(Path.Combine(UpdateDir, "manifest.json.sig"));
        Assert.Contains("No player update on this server.", Message(c.GetPlayer()));
        File.WriteAllText(Path.Combine(UpdateDir, "manifest.json.sig"), Signature);
        File.Delete(Path.Combine(UpdateDir, "manifest.json"));
        Assert.Contains("No player update on this server.", Message(c.GetPlayer()));
    }

    [Fact]
    public void Manifest_and_signature_are_returned_verbatim()
    {
        var ok = Assert.IsType<OkObjectResult>(Make().GetPlayer());
        using var doc = JsonDocument.Parse(JsonSerializer.Serialize(ok.Value));
        Assert.Equal(Manifest, doc.RootElement.GetProperty("manifest").GetString());
        Assert.Equal(Signature, doc.RootElement.GetProperty("signature").GetString());
    }

    [Fact]
    public void A_damaged_manifest_is_still_returned_verbatim_and_serves_no_files()
    {
        var c = Make(manifest: "this is not json {");
        var ok = Assert.IsType<OkObjectResult>(c.GetPlayer());
        using var doc = JsonDocument.Parse(JsonSerializer.Serialize(ok.Value));
        Assert.Equal("this is not json {", doc.RootElement.GetProperty("manifest").GetString());
        Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile("reborn.dll"));
        Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile("dxgi.dll"));
    }

    [Fact]
    public void Listed_file_is_served()
    {
        var c = Make();
        var reborn = Assert.IsType<FileContentResult>(c.GetPlayerFile("reborn.dll"));
        Assert.Equal("application/octet-stream", reborn.ContentType);
        Assert.Equal(RebornDll, reborn.FileContents);
        var dxgi = Assert.IsType<FileContentResult>(c.GetPlayerFile("dxgi.dll"));
        Assert.Equal(DxgiDll, dxgi.FileContents);
    }

    [Fact]
    public void A_whitelisted_file_missing_from_the_manifest_or_the_folder_gives_404()
    {
        string onlyReborn = "{\"version\":\"1.0.7\",\"build\":7,\"notes\":\"\",\"files\":[{\"name\":\"reborn.dll\",\"sha256\":\"aa\",\"size\":4}]}";
        var c = Make(manifest: onlyReborn);
        Assert.IsType<FileContentResult>(c.GetPlayerFile("reborn.dll"));
        Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile("dxgi.dll"));

        File.Delete(Path.Combine(UpdateDir, "reborn.dll"));
        Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile("reborn.dll"));
    }

    [Theory]
    [InlineData("reborn.pdb")]
    [InlineData("manifest.json")]
    [InlineData("manifest.json.sig")]
    [InlineData("..\\appsettings.json")]
    [InlineData("../appsettings.json")]
    [InlineData("%2e%2e%2fadmin-token.txt")]
    [InlineData("..%2fadmin-token.txt")]
    [InlineData("..\\player-update\\reborn.dll")]
    [InlineData("./reborn.dll")]
    [InlineData("REBORN.DLL")]
    [InlineData("reborn.dll ")]
    [InlineData("reborn.dll\0.txt")]
    [InlineData("")]
    [InlineData(null)]
    public void Unlisted_or_unknown_or_traversal_names_give_404(string? name)
    {
        Assert.IsType<NotFoundObjectResult>(Make().GetPlayerFile(name!));
    }

    [Fact]
    public void Admin_token_file_is_never_reachable()
    {
        // Even when the update folder itself holds one (a mis-pointed config), nothing but the two dlls goes out.
        var c = Make();
        File.WriteAllText(Path.Combine(UpdateDir, "admin-token.txt"), "SECRET-TOKEN");
        foreach (string name in new[] { "admin-token.txt", "..\\admin-token.txt", "../admin-token.txt", "..%2fadmin-token.txt", "%2e%2e%2fadmin-token.txt" })
            Assert.IsType<NotFoundObjectResult>(c.GetPlayerFile(name));
    }

    [Fact]
    public void Default_folder_is_player_update_next_to_the_exe()
    {
        var empty = new ConfigurationBuilder().Build();
        Assert.Equal(Path.Combine(AppContext.BaseDirectory, "player-update"), Updates.Folder(empty));
    }

    [Fact]
    public void Is_open_to_everyone_by_design()
    {
        var attrs = typeof(Updates).GetCustomAttributes(inherit: true).Select(a => a.GetType().Name).ToList();
        Assert.DoesNotContain("ServiceFilterAttribute", attrs);
        Assert.DoesNotContain("AuthorizeAttribute", attrs);
    }
}
