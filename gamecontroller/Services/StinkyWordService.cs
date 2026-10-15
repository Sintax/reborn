using ProfanityFilter;

namespace gamecontroller.Services
{
	public class StinkyWordService
	{
		public bool StringContainsNoNoWords(string theString)
		{
			ProfanityFilter.ProfanityFilter filter = new ProfanityFilter.ProfanityFilter();
			filter.RemoveProfanity("wog"); // GWOG IS SLUR CONFIRMED???? WTF????
			// Whole words only: the library's default also matches inside words, so "start"
			// (contains "tart") and "class" were refused as game and player names.
			return filter.DetectAllProfanities(theString, removePartialMatches: true).Count > 0;
		}
	}
}
