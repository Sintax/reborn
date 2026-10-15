using gamecontroller.Services;

namespace gamecontroller.Tests;

public class StinkyWordServiceTests
{
    // The profanity library matches inside words by default: "start" contains "tart", so the
    // default game name "start-solo's The Algorithm" was refused. Only whole words count.
    [Theory]
    [InlineData("start-solo's The Algorithm")]
    [InlineData("Start a game")]
    [InlineData("dj's Incursion - Overgrowth")]
    [InlineData("Scunthorpe assistants")]
    public void Ordinary_names_are_allowed(string name)
    {
        Assert.False(new StinkyWordService().StringContainsNoNoWords(name));
    }

    [Theory]
    [InlineData("you shit")]
    [InlineData("SHIT")]
    public void A_whole_swear_word_is_still_caught(string name)
    {
        Assert.True(new StinkyWordService().StringContainsNoNoWords(name));
    }
}
