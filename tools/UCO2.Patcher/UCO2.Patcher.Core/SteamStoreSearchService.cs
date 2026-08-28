using System.Text.Json;
using System.Text.RegularExpressions;

namespace UCO2.Patcher.Core;

public sealed partial class SteamStoreSearchService : IDisposable
{
    private readonly HttpClient client;

    public SteamStoreSearchService(HttpMessageHandler? handler = null)
    {
        client = handler is null ? new HttpClient() : new HttpClient(handler);
        client.DefaultRequestHeaders.UserAgent.ParseAdd("UCOnline2-Patcher/1.0");
        client.Timeout = TimeSpan.FromSeconds(15);
    }

    public async Task<IReadOnlyList<SteamSearchResult>> SearchAsync(string gameDirectory, CancellationToken cancellationToken = default)
    {
        string term = BuildSearchTerm(gameDirectory);
        if (term.Length == 0) return [];

        string url = $"https://store.steampowered.com/api/storesearch/?term={Uri.EscapeDataString(term)}&l=english&cc=US";
        using HttpResponseMessage response = await client.GetAsync(url, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using Stream content = await response.Content.ReadAsStreamAsync(cancellationToken);
        return ParseResults(content);
    }

    public static string BuildSearchTerm(string gameDirectory)
    {
        string folder = Path.GetFileName(Path.TrimEndingDirectorySeparator(gameDirectory));
        string term = SeparatorsRegex().Replace(folder, " ");
        term = ReleaseSuffixRegex().Replace(term, "");
        return WhitespaceRegex().Replace(term, " ").Trim();
    }

    public static IReadOnlyList<SteamSearchResult> ParseResults(Stream json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        if (!document.RootElement.TryGetProperty("items", out JsonElement items) || items.ValueKind != JsonValueKind.Array)
            return [];

        var results = new List<SteamSearchResult>();
        foreach (JsonElement item in items.EnumerateArray())
        {
            if (!item.TryGetProperty("id", out JsonElement idElement) || !idElement.TryGetUInt32(out uint appId)) continue;
            if (!item.TryGetProperty("name", out JsonElement nameElement)) continue;
            string? name = nameElement.GetString();
            if (string.IsNullOrWhiteSpace(name)) continue;
            string? image = item.TryGetProperty("tiny_image", out JsonElement imageElement) ? imageElement.GetString() : null;
            results.Add(new SteamSearchResult(appId, name, image));
        }
        return results;
    }

    public static SteamSearchResult? FindBestMatch(string gameDirectory, IReadOnlyList<SteamSearchResult> results)
    {
        if (results.Count == 0) return null;
        string wanted = Normalize(BuildSearchTerm(gameDirectory));
        return results.FirstOrDefault(result => Normalize(result.Name) == wanted) ?? results[0];
    }

    private static string Normalize(string value) => new(value.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());

    public void Dispose() => client.Dispose();

    [GeneratedRegex("[._-]+")]
    private static partial Regex SeparatorsRegex();

    [GeneratedRegex(@"\s+(?:build|version|v)\s*\d[\w.\-]*.*$|\s+(?:p2p|portable|repack)$", RegexOptions.IgnoreCase)]
    private static partial Regex ReleaseSuffixRegex();

    [GeneratedRegex(@"\s+")]
    private static partial Regex WhitespaceRegex();
}
