namespace UCO2.Patcher.Core;

public sealed record MissingBackendSettings(string Backend, IReadOnlyList<string> Fields);

public static class BackendSettingsValidator
{
    public static IReadOnlyList<MissingBackendSettings> FindMissing(GameScanResult game, PatchOptions options)
    {
        var missing = new List<MissingBackendSettings>();
        if (options.InstallPhoton)
        {
            var fields = new List<string>();
            if (game.Backends.HasFlag(BackendKind.PhotonFusion))
            {
                if (string.IsNullOrWhiteSpace(options.PhotonFusionAppId)) fields.Add("Fusion AppId");
            }
            else if (game.Backends.HasFlag(BackendKind.PhotonRealtime))
            {
                if (string.IsNullOrWhiteSpace(options.PhotonRealtimeAppId)) fields.Add("Realtime AppId");
            }
            else if (string.IsNullOrWhiteSpace(options.PhotonRealtimeAppId) && string.IsNullOrWhiteSpace(options.PhotonFusionAppId))
            {
                fields.Add("Realtime or Fusion AppId");
            }
            if (game.Backends.HasFlag(BackendKind.PhotonVoice) && string.IsNullOrWhiteSpace(options.PhotonVoiceAppId))
                fields.Add("Voice AppId");
            if (fields.Count > 0) missing.Add(new MissingBackendSettings("Photon", fields));
        }

        // KeepGameApp uses the game's own Epic app -- no credentials required.
        if (options.InstallEos && !options.EosKeepGameApp)
        {
            var fields = new List<string>();
            AddIfMissing(fields, "ProductId", options.EosProductId);
            AddIfMissing(fields, "SandboxId", options.EosSandboxId);
            AddIfMissing(fields, "DeploymentId", options.EosDeploymentId);
            AddIfMissing(fields, "ClientId", options.EosClientId);
            AddIfMissing(fields, "ClientSecret", options.EosClientSecret);
            if (fields.Count > 0) missing.Add(new MissingBackendSettings("EOS", fields));
        }

        // KeepGameTitle uses the game's own PlayFab title -- no TitleId required.
        if (options.InstallPlayFab && !options.PlayFabKeepGameTitle && string.IsNullOrWhiteSpace(options.PlayFabTitleId))
            missing.Add(new MissingBackendSettings("PlayFab", ["TitleId"]));
        return missing;
    }

    private static void AddIfMissing(List<string> fields, string name, string value)
    {
        if (string.IsNullOrWhiteSpace(value)) fields.Add(name);
    }
}
