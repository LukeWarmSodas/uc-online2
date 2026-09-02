using System.Text;
using System.Text.RegularExpressions;

namespace UCO2.Patcher.Core;

public static partial class ConfigBuilder
{
    public static string Build(GameScanResult game, PatchOptions options)
    {
        var output = new StringBuilder();
        output.AppendLine("[Settings]");
        output.AppendLine($"AppId={options.AppId}");
        output.AppendLine($"ogAppId={options.OriginalAppId}");
        output.AppendLine("PluginsFolder=plugins");
        output.AppendLine($"GetStubbedLol={Bool(options.EnableSteamStub)}");
        output.AppendLine($"LoadOverlay={Bool(options.LoadOverlay)}");
        output.AppendLine($"LogOverlay={Bool(options.LogOverlay)}");
        output.AppendLine($"WarnOverlayDisabled={Bool(options.WarnOverlayDisabled)}");
        output.AppendLine($"VerboseLog={Bool(options.VerboseLog)}");
        output.AppendLine($"ForceOwnership={Bool(options.ForceOwnership)}");
        output.AppendLine($"PassthroughTicket={Bool(options.PassthroughTicket)}");
        output.AppendLine($"EmulateTicket={Bool(options.EmulateTicket)}");
        output.AppendLine($"SDR={Bool(options.EnableSdr)}");
        output.AppendLine($"InventoryAutoGrant={Bool(options.InventoryAutoGrant)}");
        if (!string.IsNullOrWhiteSpace(options.LegacyClientVersion))
            output.AppendLine($"Client={SingleLine(options.LegacyClientVersion)}");
        foreach ((string key, string value) in options.AdditionalSettings.OrderBy(pair => pair.Key, StringComparer.OrdinalIgnoreCase))
            output.AppendLine($"{SingleLine(key)}={SingleLine(value)}");

        output.AppendLine();
        output.AppendLine("[DLC]");
        output.AppendLine($"UnlockAll={Bool(options.UnlockAllDlc)}");
        foreach ((uint appId, string name) in FindDlcEntries(game))
            output.AppendLine($"{appId}={SingleLine(name)}");

        if (options.InstallPhoton)
        {
            output.AppendLine();
            bool fusion = game.Backends.HasFlag(BackendKind.PhotonFusion);
            output.AppendLine(fusion ? "[Fusion]" : "[Realtime]");
            if (fusion)
                output.AppendLine($"PhotonAppIdFusion={SingleLine(options.PhotonFusionAppId)}");
            else
                output.AppendLine($"PhotonAppIdRealtime={SingleLine(options.PhotonRealtimeAppId)}");
            if (!string.IsNullOrWhiteSpace(options.PhotonVoiceAppId))
                output.AppendLine($"PhotonAppIdVoice={SingleLine(options.PhotonVoiceAppId)}");
            output.AppendLine("ForcedAuthType=0");
        }

        if (options.InstallEos)
        {
            output.AppendLine();
            output.AppendLine("[EOS]");
            if (options.EosKeepGameApp)
            {
                // Device ID login on the game's OWN Epic app -- no app credentials needed.
                output.AppendLine("KeepGameApp=1");
            }
            else
            {
                output.AppendLine($"ProductId={SingleLine(options.EosProductId)}");
                output.AppendLine($"SandboxId={SingleLine(options.EosSandboxId)}");
                output.AppendLine($"DeploymentId={SingleLine(options.EosDeploymentId)}");
                output.AppendLine($"ClientId={SingleLine(options.EosClientId)}");
                output.AppendLine($"ClientSecret={SingleLine(options.EosClientSecret)}");
            }
            output.AppendLine($"DisplayName={SingleLine(options.DisplayName)}");
        }

        if (options.InstallCoherence)
        {
            output.AppendLine();
            output.AppendLine("[Coherence]");
            output.AppendLine("ForceGuestLogin=true");
            output.AppendLine($"RuntimeKey={SingleLine(options.CoherenceRuntimeKey)}");
            output.AppendLine("LocalMode=false");
        }

        if (options.InstallPlayFab)
        {
            output.AppendLine();
            output.AppendLine("[PlayFab]");
            if (options.PlayFabKeepGameTitle)
                output.AppendLine("KeepGameTitle=1");
            if (!string.IsNullOrWhiteSpace(options.PlayFabTitleId))
                output.AppendLine($"TitleId={SingleLine(options.PlayFabTitleId)}");
        }

        return output.ToString().Replace("\r\n", "\n").Replace("\n", Environment.NewLine);
    }

    private static IReadOnlyList<(uint AppId, string Name)> FindDlcEntries(GameScanResult game)
    {
        string? config = SafeFileSystem.EnumerateFiles(game.GameDirectory, "configs.app.ini").FirstOrDefault();
        var entries = new Dictionary<uint, string>();
        if (config is not null)
        {
            try
            {
                foreach (string line in File.ReadLines(config))
                {
                    Match match = DlcLineRegex().Match(line);
                    if (match.Success && uint.TryParse(match.Groups["id"].Value, out uint appId))
                        entries[appId] = match.Groups["name"].Value.Trim();
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }

        if (entries.Count == 0)
        {
            try
            {
                foreach (string directory in Directory.EnumerateDirectories(game.ConfigDirectory, "*", SearchOption.TopDirectoryOnly))
                {
                    if (uint.TryParse(Path.GetFileName(directory), out uint appId))
                        entries[appId] = $"DLC {appId}";
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
        return entries.Select(pair => (pair.Key, pair.Value)).ToArray();
    }

    private static string SingleLine(string? value) => (value ?? "").Replace("\r", "").Replace("\n", "").Trim();
    private static string Bool(bool value) => value ? "true" : "false";

    [GeneratedRegex("^\\s*(?<id>[0-9]+)\\s*=\\s*(?<name>.*)$")]
    private static partial Regex DlcLineRegex();
}
