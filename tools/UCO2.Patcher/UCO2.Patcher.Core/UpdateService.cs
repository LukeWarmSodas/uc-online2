using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace UCO2.Patcher.Core;

public sealed class UpdateService : IDisposable
{
    private readonly HttpClient client;

    public UpdateService()
    {
        client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("UCOnline2-Patcher", "1.0"));
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
    }

    public async Task<ReleaseInfo> GetLatestAsync(CancellationToken cancellationToken = default)
    {
        using HttpResponseMessage response = await client.GetAsync(
            "https://api.github.com/repos/LukeWarmSodas/uc-online2/releases/latest",
            cancellationToken);
        response.EnsureSuccessStatusCode();
        await using Stream stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using JsonDocument document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        JsonElement root = document.RootElement;

        ReleaseAsset? releaseArchive = null;
        foreach (JsonElement asset in root.GetProperty("assets").EnumerateArray())
        {
            string name = asset.GetProperty("name").GetString() ?? "";
            if (!name.EndsWith("-release.zip", StringComparison.OrdinalIgnoreCase))
                continue;
            releaseArchive = new ReleaseAsset(
                name,
                asset.GetProperty("browser_download_url").GetString() ?? "",
                asset.TryGetProperty("digest", out JsonElement digest) ? digest.GetString() : null,
                asset.GetProperty("size").GetInt64());
            break;
        }

        return new ReleaseInfo(
            root.GetProperty("tag_name").GetString() ?? "",
            root.GetProperty("name").GetString() ?? "",
            root.GetProperty("html_url").GetString() ?? "",
            root.GetProperty("prerelease").GetBoolean(),
            releaseArchive);
    }

    public async Task<string> DownloadAsync(ReleaseAsset asset, IProgress<double>? progress = null, CancellationToken cancellationToken = default)
    {
        string downloadDirectory = Path.Combine(Path.GetTempPath(), "UCOnline2", "updates", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(downloadDirectory);
        string destination = Path.Combine(downloadDirectory, asset.Name);

        using HttpResponseMessage response = await client.GetAsync(asset.DownloadUrl, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        long total = response.Content.Headers.ContentLength ?? asset.Size;
        await using Stream input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using FileStream output = File.Create(destination);
        byte[] buffer = new byte[128 * 1024];
        long copied = 0;
        int read;
        while ((read = await input.ReadAsync(buffer, cancellationToken)) > 0)
        {
            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            copied += read;
            if (total > 0) progress?.Report(copied / (double)total);
        }
        await output.FlushAsync(cancellationToken);

        if (!string.IsNullOrWhiteSpace(asset.Digest) && asset.Digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
        {
            string expected = asset.Digest[7..];
            string actual = SafeFileSystem.Sha256(destination);
            if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The downloaded update failed SHA-256 verification.");
        }
        return destination;
    }

    public static bool IsNewer(string current, string available)
    {
        if (current.Equals("development", StringComparison.OrdinalIgnoreCase))
            return false;
        return ParseVersion(available) > ParseVersion(current);
    }

    private static Version ParseVersion(string value)
    {
        string numeric = value.Trim().TrimStart('v', 'V').Split('-', 2)[0];
        return Version.TryParse(numeric, out Version? version) ? version : new Version(0, 0);
    }

    public void Dispose() => client.Dispose();
}

public static class SelfUpdateService
{
    public static ProcessStartInfo CreateUpdaterStartInfo(string archivePath, string installDirectory, string executableName, int parentProcessId, string? restartGameDirectory)
    {
        string currentExecutable = Environment.ProcessPath
            ?? throw new InvalidOperationException("The current executable path is unavailable.");
        string updaterDirectory = Path.Combine(Path.GetTempPath(), "UCOnline2", "updater", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(updaterDirectory);
        string updaterExecutable = Path.Combine(updaterDirectory, "UCO2.Patcher.Updater.exe");
        File.Copy(currentExecutable, updaterExecutable);

        return new ProcessStartInfo
        {
            FileName = updaterExecutable,
            UseShellExecute = true,
            Arguments = string.Join(' ', new[]
            {
                "--apply-update",
                Quote(archivePath),
                Quote(installDirectory),
                Quote(executableName),
                parentProcessId.ToString(),
                Quote(restartGameDirectory ?? "")
            })
        };
    }

    public static async Task ApplyAsync(string archivePath, string installDirectory, string executableName, int parentProcessId, string? restartGameDirectory)
    {
        try
        {
            using Process? parent = Process.GetProcessById(parentProcessId);
            await parent.WaitForExitAsync();
        }
        catch (ArgumentException) { }

        string staging = Path.Combine(Path.GetTempPath(), "UCOnline2", "staging", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        ZipFile.ExtractToDirectory(archivePath, staging);
        string? packagedExecutable = SafeFileSystem.EnumerateFiles(staging, executableName).FirstOrDefault();
        if (packagedExecutable is null)
            throw new InvalidDataException($"The update archive does not contain {executableName}.");
        string packageRoot = Path.GetDirectoryName(packagedExecutable)!;

        foreach (string source in SafeFileSystem.EnumerateFiles(packageRoot))
        {
            string relative = Path.GetRelativePath(packageRoot, source);
            string target = Path.Combine(installDirectory, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(source, target, overwrite: true);
        }

        var start = new ProcessStartInfo
        {
            FileName = Path.Combine(installDirectory, executableName),
            WorkingDirectory = installDirectory,
            UseShellExecute = true
        };
        if (!string.IsNullOrWhiteSpace(restartGameDirectory))
            start.Arguments = $"--game {Quote(restartGameDirectory)} --refresh-fix";
        Process.Start(start);
    }

    private static string Quote(string value) => $"\"{value.Replace("\"", "\\\"")}\"";
}
