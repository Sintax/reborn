using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc.Filters;
using System.Threading.Tasks;

namespace gamecontroller.Middleware
{
	// You may need to install the Microsoft.AspNetCore.Http.Abstractions package into your project
	public class RCONAuth
	{
		// Without RCON_TOKEN nothing is RCON-authorised; the service still runs (telemetry posts just land nowhere).
		private readonly string? _RCONToken;

		public RCONAuth()
		{
			_RCONToken = Environment.GetEnvironmentVariable("RCON_TOKEN");
		}

		public async Task<bool> HasValidRCONAuth(HttpContext context)
		{
			if (!context.Request.Headers.TryGetValue("Authorization", out var authHeader))
			{
				return false;
			}

			var token = authHeader.ToString();

			if (token.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase))
			{
				token = token.Substring(7);
			}

			if (string.IsNullOrEmpty(token) || _RCONToken == null || !string.Equals(token, _RCONToken, StringComparison.Ordinal))
			{
				return false;
			}

			return true;
		}
	}

	// Extension method used to add the middleware to the HTTP request pipeline.
	public static class HasRCONAuthExtensions
	{
		public static IApplicationBuilder UseHasRCONAuth(this IApplicationBuilder builder)
		{
			return builder.UseMiddleware<RCONAuth>();
		}
	}
}
