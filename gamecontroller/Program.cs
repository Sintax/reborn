using gamecontroller.Controllers;
using gamecontroller.Models;
using gamecontroller.Services;
using gamecontroller.Singletons;

// Settings (appsettings.json) come from the service's own folder, wherever it is started from.
var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = args, ContentRootPath = AppContext.BaseDirectory });

// appsettings.json is re-read when it changes (start keys apply without a restart). A reload that
// catches the file half-written or hand-edited into bad JSON must not take the service down: it is
// ignored after startup, while a broken file at startup still stops the service with the error.
var started = false;
foreach (var source in builder.Configuration.Sources.OfType<Microsoft.Extensions.Configuration.Json.JsonConfigurationSource>())
    source.OnLoadException = ctx => ctx.Ignore = started;

builder.Logging.AddProvider(new FileLoggerProvider(Path.Combine(AppContext.BaseDirectory, "matchmaking.log")));

// Add services to the container.

builder.Services.AddControllers();

builder.Services.AddSingleton<IServerLauncher, LoaderServerLauncher>();

builder.Services.AddSingleton<DatabaseSessions>();

builder.Services.AddSingleton<StinkyWordService>();

builder.Services.AddSingleton<UserList>();

// When each player name was last seen in a game; disposed (and so saved) when the service stops.
builder.Services.AddSingleton(sp => new SeenLog(SeenLog.PathFor(builder.Configuration), () => DateTime.UtcNow,
    sp.GetRequiredService<ILogger<SeenLog>>()));

// The servers the service keeps running for the game's server browser: "Roster" in appsettings.json.
builder.Services.AddSingleton(sp =>
{
    var slots = builder.Configuration.GetSection("Roster").Get<List<RosterSlot>>() ?? new List<RosterSlot>();
    var ports = builder.Configuration.GetSection("GamePorts").Get<List<int>>() ?? new List<int> { 7777, 7778, 7779 };
    var requested = builder.Configuration.GetSection("RequestedGame").Get<RequestedGameSettings>() ?? new RequestedGameSettings();
    return new ServerRoster(slots, sp.GetRequiredService<IServerLauncher>(), ports, "localhost:" + (builder.Configuration["CoordinatorPort"] ?? "5000"), requested);
});

builder.Services.AddHostedService<RosterService>();

// The local admin API for the Control Panel (Controllers/Admin.cs): same machine only, with the token
// in admin-token.txt next to the service (made on the first start, then kept). Ports are saved into
// this service's own appsettings.json for the next start.
builder.Services.AddSingleton(new ServiceStarted(DateTime.UtcNow));
// A token file that can't be read or made only shuts the panel out; matchmaking runs on.
builder.Services.AddSingleton(sp => AdminToken.StartUp(Path.Combine(AppContext.BaseDirectory, AdminToken.FileName),
    sp.GetRequiredService<ILogger<AdminToken>>()));
builder.Services.AddSingleton<AdminGuard>();
builder.Services.AddSingleton(_ => new UserListWriter(builder.Configuration));
builder.Services.AddSingleton(_ => new SettingsWriter(Path.Combine(AppContext.BaseDirectory, "appsettings.json")));

var app = builder.Build();
started = true;

// Make the admin token file now, not on the panel's first call (it is never logged).
app.Services.GetRequiredService<AdminToken>();

// First start after the switch from StartKeys: the keys become users. Never overwrites a user list.
UserList.Migrate(app.Configuration, UserList.PathFor(app.Configuration), app.Services.GetRequiredService<ILoggerFactory>().CreateLogger<UserList>());

// Configure the HTTP request pipeline.
// Plain HTTP on purpose: the game's httplib client talks HTTP, and a redirect would break it.

app.UseAuthorization();

app.MapControllers();

app.Run();
