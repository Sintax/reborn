using gamecontroller.Models;
using System.Diagnostics;

namespace gamecontroller.Singletons
{
	public class GameSessions
	{
		private readonly LobbySingleton _lobbySingleton;
		private readonly IServerLauncher _launcher;

		public List<int> ports = [7777, 7778, 7779];

		public GameSessions(LobbySingleton lobbySingleton, IServerLauncher launcher)
		{
			_lobbySingleton = lobbySingleton;
			_launcher = launcher;
		}

		public List<int> GetAvailablePorts()
		{
			List<int> retPorts = new List<int>();

			foreach (int port in ports)
			{
				bool shouldAdd = true;

				foreach (Lobby lobby in _lobbySingleton.Lobbies)
				{
					if(lobby.GameInstance != null && lobby.GameInstance.Port == port)
					{
						shouldAdd = false;
						break;
					}
				}

				if (shouldAdd)
				{
					retPorts.Add(port);
				}
			}

			return retPorts;
		}

        private async Task<string> GetPublicIp()
        {
            HttpClient httpClient = new HttpClient();

            var response = await httpClient.GetAsync("https://checkip.amazonaws.com");

            return (await response.Content.ReadAsStringAsync()).Replace("\n", "");
        }

        public async Task LaunchNewGame(Lobby lobbyToLaunch)
		{
			GameCreationConfig config = new GameCreationConfig(lobbyToLaunch);

			string ip = await GetPublicIp();

			int port = GetAvailablePorts().FirstOrDefault();

			if(port != 0)
			{
				GameInstance gameInstance = new GameInstance();

				gameInstance.Port = port;

				// Same launch options as the roster servers (ServerRoster.cs); the map comes from server-config.
				gameInstance.PID = _launcher.Launch("rb-lobby-" + port, "-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbinstance=rb-lobby-" + port
					+ " -rbcoordinator=localhost:5000 -rbcoordinatorkey=" + gameInstance.MyGuid);

				gameInstance.ConnectionString = "open " + await GetPublicIp() + ":" + port.ToString();

				gameInstance.Config = config;

				lobbyToLaunch.GameInstance = gameInstance;

				lobbyToLaunch.ServerLaunching();
			}
		}
	}
}
