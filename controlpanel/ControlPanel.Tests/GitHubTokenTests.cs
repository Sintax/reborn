using System.Text;
using ControlPanel.Core;

namespace ControlPanel.Tests
{
	// Every test keeps its token in its own temp folder: the real %LOCALAPPDATA%\Reborn\builds-token.dat is
	// never touched, and the values are dummies.
	public sealed class GitHubTokenTests : IDisposable
	{
		private const string Dummy = "ghp_DUMMY_not_a_real_token_0123456789";
		private readonly string _dir = Path.Combine(Path.GetTempPath(), "cp-token-tests-" + Guid.NewGuid().ToString("N"));

		public GitHubTokenTests() => Directory.CreateDirectory(_dir);

		private string KeptPath => Path.Combine(_dir, "Reborn", GitHubToken.FileName);

		public void Dispose()
		{
			try { Directory.Delete(_dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
		}

		[Fact]
		public void token_round_trips_and_is_not_plain_text_on_disk()
		{
			Assert.True(GitHubToken.Save("  " + Dummy + "\r\n", KeptPath));   // trimmed

			Assert.Equal(Dummy, GitHubToken.Load(KeptPath));
			byte[] onDisk = File.ReadAllBytes(KeptPath);
			Assert.DoesNotContain(Dummy, Encoding.UTF8.GetString(onDisk));
			Assert.DoesNotContain(Dummy, Encoding.Unicode.GetString(onDisk));
			Assert.False(new ReadOnlySpan<byte>(onDisk).IndexOf(Encoding.UTF8.GetBytes(Dummy)) >= 0);
		}

		[Fact]
		public void empty_token_is_not_saved()
		{
			Assert.False(GitHubToken.Save("", KeptPath));
			Assert.False(GitHubToken.Save("  \r\n ", KeptPath));
			Assert.False(GitHubToken.Save(null, KeptPath));
			Assert.False(File.Exists(KeptPath));
			Assert.Null(GitHubToken.Load(KeptPath));

			// An empty answer later leaves the token already kept alone.
			GitHubToken.Save(Dummy, KeptPath);
			GitHubToken.Save("", KeptPath);
			Assert.Equal(Dummy, GitHubToken.Load(KeptPath));
		}

		[Fact]
		public void a_damaged_or_foreign_file_loads_as_no_token()
		{
			Directory.CreateDirectory(Path.GetDirectoryName(KeptPath)!);
			File.WriteAllText(KeptPath, "not scrambled at all");
			Assert.Null(GitHubToken.Load(KeptPath));
			Assert.Null(GitHubToken.Load(Path.Combine(_dir, "missing.dat")));
		}

		[Fact]
		public void save_token_cli_deletes_the_file()
		{
			string file = Path.Combine(_dir, "handover.tmp");
			File.WriteAllText(file, Dummy + "\r\n");

			Assert.Equal(file, GitHubToken.SwitchFile(new[] { "--save-github-token", file }));
			Assert.Equal(0, GitHubToken.SaveFromFile(file, KeptPath));

			Assert.False(File.Exists(file));
			Assert.Equal(Dummy, GitHubToken.Load(KeptPath));
		}

		[Fact]
		public void save_token_cli_deletes_the_file_even_when_nothing_is_kept()
		{
			string empty = Path.Combine(_dir, "empty.tmp");
			File.WriteAllText(empty, "  \r\n");
			Assert.Equal(0, GitHubToken.SaveFromFile(empty, KeptPath));   // nothing to keep is not a failure
			Assert.False(File.Exists(empty));
			Assert.False(File.Exists(KeptPath));

			// The token can't be kept (the target folder is really a file): the hand-over file still goes.
			string blocker = Path.Combine(_dir, "blocker");
			File.WriteAllText(blocker, "x");
			string file = Path.Combine(_dir, "handover.tmp");
			File.WriteAllText(file, Dummy);
			Assert.Equal(1, GitHubToken.SaveFromFile(file, Path.Combine(blocker, "x.dat")));
			Assert.False(File.Exists(file));

			Assert.Equal(1, GitHubToken.SaveFromFile(Path.Combine(_dir, "missing.tmp"), KeptPath));
		}

		[Fact]
		public void only_the_save_switch_is_the_token_command()
		{
			Assert.Null(GitHubToken.SwitchFile(Array.Empty<string>()));
			Assert.Null(GitHubToken.SwitchFile(new[] { "--other" }));
			Assert.Equal("", GitHubToken.SwitchFile(new[] { "--save-github-token" }));   // no file: fails, shows no window
		}
	}
}
