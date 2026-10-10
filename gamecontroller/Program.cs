using gamecontroller.Middleware;
using gamecontroller.Services;
using gamecontroller.Singletons;

var builder = WebApplication.CreateBuilder(args);

// Add services to the container.

builder.Services.AddControllers();

builder.Services.AddScoped<AuthService>();

builder.Services.AddScoped<PlayerAuthActionFilter>();

builder.Services.AddSingleton<IServerLauncher, LoaderServerLauncher>();

builder.Services.AddSingleton<GameSessions>();

builder.Services.AddSingleton<DatabaseSessions>();

builder.Services.AddSingleton<LobbySingleton>();

builder.Services.AddSingleton<StinkyWordService>();

// The servers the service keeps running for the game's server browser: "Roster" in appsettings.json.
builder.Services.AddSingleton(sp =>
{
    var slots = builder.Configuration.GetSection("Roster").Get<List<RosterSlot>>() ?? new List<RosterSlot>();
    var ports = builder.Configuration.GetSection("GamePorts").Get<List<int>>() ?? new List<int> { 7777, 7778, 7779 };
    return new ServerRoster(slots, sp.GetRequiredService<IServerLauncher>(), ports, "localhost:" + (builder.Configuration["CoordinatorPort"] ?? "5000"));
});

builder.Services.AddHostedService<SessionCleanupService>();

builder.Services.AddHostedService<MatchLaunchService>();

builder.Services.AddHostedService<WebsocketOutgoingService>();

builder.Services.AddHostedService<RosterService>();

var app = builder.Build();

// Configure the HTTP request pipeline.
// Plain HTTP on purpose: the game's httplib client talks HTTP, and a redirect would break it.

app.UseAuthorization();

app.UseWebSockets();

app.MapControllers();

app.Run();
