using System.Text;

namespace Reborn.Update.Tests;

public sealed class CheckTests
{
    // Made by tools/signing.py with a throw-away key (never the owner's key):
    //   keygen(<scratch>/test-key.pem); sign(Message, <scratch>/test-key.pem)
    const string TestPublicKey = "DzTz+f5A3v/It8UJdGRCyCNOCIZJ9wOl288T9PO/yAH0q7Mra29Pa4DexYOPDLjCUO8yWLJSEMA/NL0Fs0MPYQ==";
    const string Message = "Battleborn Reborn test update 1.0.7";
    const string MessageSignature = "YIo/c/ONDTiZ9J/RcQpYyNFf7FptqAj2rwy+heDh7dCBtOlpeBF6+RoS5SN1r3sfdkyuBW24NWXAtjheAc8gBg==";

    [Fact]
    public void Signature_matches_python()
    {
        byte[] data = Encoding.UTF8.GetBytes(Message);
        Assert.True(Signature.Verify(data, MessageSignature, TestPublicKey));

        Assert.False(Signature.Verify(Encoding.UTF8.GetBytes(Message + "!"), MessageSignature, TestPublicKey));
        Assert.False(Signature.Verify(data, MessageSignature, UpdateKey.PublicKey));
        Assert.False(Signature.Verify(data, "not base64 at all", TestPublicKey));
        Assert.False(Signature.Verify(data, Convert.ToBase64String(new byte[63]), TestPublicKey));
        Assert.False(Signature.Verify(data, MessageSignature, Convert.ToBase64String(new byte[64])));
        Assert.False(Signature.Verify(data, MessageSignature, ""));
        Assert.Equal(64, Convert.FromBase64String(UpdateKey.PublicKey).Length);
    }

    const string FullManifest = """
        {"version":"1.0.3","build":3,"notes":"Bots fixed.","commit":"7984e6e","zip":"Reborn-Server-Update-1.0.3.zip","sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08","size":1234}
        """;

    [Fact]
    public void Manifest_rejects_missing_fields()
    {
        UpdateManifest? m = UpdateManifest.Parse(FullManifest);
        Assert.Equal(new UpdateManifest("1.0.3", 3, "Bots fixed.", "7984e6e", "Reborn-Server-Update-1.0.3.zip",
            "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", 1234), m);

        foreach (string field in new[] { "version", "build", "notes", "commit", "zip", "sha256", "size" })
        {
            var obj = System.Text.Json.Nodes.JsonNode.Parse(FullManifest)!.AsObject();
            obj.Remove(field);
            Assert.True(UpdateManifest.Parse(obj.ToJsonString()) is null, field);
        }

        Assert.Null(UpdateManifest.Parse(""));
        Assert.Null(UpdateManifest.Parse("not json"));
        Assert.Null(UpdateManifest.Parse("[]"));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("\"build\":3", "\"build\":\"3\"")));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("\"build\":3", "\"build\":4")));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("\"1.0.3\"", "\"7984e6e\"").Replace("\"build\":3", "\"build\":0")));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("Reborn-Server-Update-1.0.3.zip", @"..\\evil.zip")));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("9f86d081", "nothex!!")));
        Assert.Null(UpdateManifest.Parse(FullManifest.Replace("\"size\":1234", "\"size\":0")));
    }

    [Fact]
    public void Sha_version_counts_as_build_zero()
    {
        Assert.Equal(7, Versions.Parse("1.0.7"));
        Assert.Equal(12, Versions.Parse(" 1.0.12\r\n"));
        Assert.Equal(0, Versions.Parse("7984e6e"));
        Assert.Equal(0, Versions.Parse("7984e6e1c2d3b4a5f60718293a4b5c6d7e8f9012"));
        Assert.Equal(0, Versions.Parse(""));
        Assert.Equal(0, Versions.Parse("1.0.03"));
        Assert.Equal(0, Versions.Parse("1.0.0"));
        Assert.Equal(0, Versions.Parse("2.0.1"));
        Assert.Equal(0, Versions.Parse("1.0.7-beta"));
        Assert.Equal(0, Versions.Parse("1.0.\u0661"));
        Assert.Equal(0, Versions.Parse(null!));

        Assert.True(Versions.Newer(1, 0));
        Assert.True(Versions.Newer(4, 3));
        Assert.False(Versions.Newer(3, 3));
        Assert.False(Versions.Newer(2, 3));
    }
}
