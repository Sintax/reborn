using gamecontroller.Services;
using Microsoft.Extensions.Configuration;

namespace gamecontroller.Tests;

public class StartKeysTests
{
    static StartKeys Make(params (string label, string key)[] keys)
    {
        var dict = keys.ToDictionary(k => "StartKeys:" + k.label, k => (string?)k.key);
        var config = new ConfigurationBuilder().AddInMemoryCollection(dict).Build();
        return new StartKeys(config);
    }

    [Fact]
    public void LabelFor_returns_the_owner_of_a_known_key()
    {
        var keys = Make(("dj", "ABCDEFGHJKMNPQRSTVWXYZ23"), ("mike", "ZYXWVTSRQPNMKJHGFEDCBA32"));
        Assert.Equal("mike", keys.LabelFor("ZYXWVTSRQPNMKJHGFEDCBA32"));
        Assert.Equal(2, keys.Count);
    }

    [Fact]
    public void LabelFor_is_null_for_unknown_empty_or_near_miss_keys()
    {
        var keys = Make(("dj", "ABCDEFGHJKMNPQRSTVWXYZ23"));
        Assert.Null(keys.LabelFor("abcdefghjkmnpqrstvwxyz23"));   // case matters
        Assert.Null(keys.LabelFor("ABCDEFGHJKMNPQRSTVWXYZ2"));
        Assert.Null(keys.LabelFor(""));
        Assert.Null(keys.LabelFor(null));
    }

    [Fact]
    public void Keys_added_or_removed_in_the_settings_apply_without_a_restart()
    {
        var config = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?> { ["StartKeys:dj"] = "ABCDEFGHJKMNPQRSTVWXYZ23" })
            .Build();
        var keys = new StartKeys(config);

        config["StartKeys:mike"] = "ZYXWVTSRQPNMKJHGFEDCBA32";
        config["StartKeys:dj"] = null;
        config.Reload();   // what the settings-file watcher does after an edit

        Assert.Equal("mike", keys.LabelFor("ZYXWVTSRQPNMKJHGFEDCBA32"));
        Assert.Null(keys.LabelFor("ABCDEFGHJKMNPQRSTVWXYZ23"));
        Assert.Equal(1, keys.Count);
    }

    [Fact]
    public void No_StartKeys_section_means_no_keys()
    {
        var config = new ConfigurationBuilder().Build();
        Assert.Equal(0, new StartKeys(config).Count);
        Assert.Null(new StartKeys(config).LabelFor("anything"));
    }
}
