namespace UCO2.Patcher.Core;

public sealed class PatchPlanner(ArtifactLocator artifacts)
{
    public PatchPlan Create(GameScanResult game, PatchOptions options)
    {
        if (options.AppId == 0) throw new InvalidOperationException("AppId must be greater than zero.");
        if (options.OriginalAppId == 0) throw new InvalidOperationException("Enter the game's real Steam AppId.");
        if (options.EnableSdr && options.OriginalAppId == 0) throw new InvalidOperationException("SDR requires ogAppId.");

        var operations = new List<PatchOperation>();
        var warnings = new List<string>(game.Warnings);
        string? steamSource = artifacts.FindSteamApi(game.Architecture);
        if (steamSource is null)
            throw new FileNotFoundException($"The {game.Architecture} UCOnline2 Steam API build was not found beside the patcher.");

        operations.Add(new PatchOperation
        {
            Kind = PatchOperationKind.ReplaceFile,
            SourcePath = steamSource,
            TargetPath = game.SteamApiPath,
            Description = $"Install {Path.GetFileName(game.SteamApiPath)}"
        });

        operations.Add(new PatchOperation
        {
            Kind = PatchOperationKind.WriteText,
            TextContent = ConfigBuilder.Build(game, options),
            TargetPath = Path.Combine(game.ConfigDirectory, "union-crax.ini"),
            Description = "Write union-crax.ini"
        });

        if (options.InstallOverlayProxy && game.Architecture == GameArchitecture.X64)
            AddOverlay(game, operations, warnings);

        if (game.Architecture == GameArchitecture.X64)
        {
            if (options.InstallPhoton) AddPlugin("photon_universal", game, operations);
            if (options.InstallEos)
            {
                bool complete = !string.IsNullOrWhiteSpace(options.EosProductId)
                    && !string.IsNullOrWhiteSpace(options.EosSandboxId)
                    && !string.IsNullOrWhiteSpace(options.EosDeploymentId)
                    && !string.IsNullOrWhiteSpace(options.EosClientId)
                    && !string.IsNullOrWhiteSpace(options.EosClientSecret);
                if (complete) AddPlugin("EOS_custom", game, operations);
                else warnings.Add("EOS was selected but its credentials are incomplete, so EOS_custom will not be installed.");
            }
            if (options.InstallPlayFab) AddPlugin("playfab_universal", game, operations);
            if (options.InstallCoherence) AddPlugin("coherence_universal", game, operations);
        }
        else if (options.InstallPhoton || options.InstallEos || options.InstallPlayFab || options.InstallCoherence)
        {
            warnings.Add("Plugins are currently x64-only and will be skipped for this 32-bit game.");
        }

        if (options.InstallCoherence && !string.IsNullOrWhiteSpace(options.CoherenceRuntimeKey) && game.UnityDataDirectory is not null)
        {
            string assets = Path.Combine(game.UnityDataDirectory, "globalgamemanagers.assets");
            if (File.Exists(assets))
            {
                operations.Add(new PatchOperation
                {
                    Kind = PatchOperationKind.WriteBytes,
                    BinaryContent = CoherenceKeyPatcher.CreatePatchedCopy(assets, options.CoherenceRuntimeKey),
                    TargetPath = assets,
                    Description = "Patch coherence runtime key"
                });
            }
            else warnings.Add("globalgamemanagers.assets was not found, so the coherence runtime key cannot be patched automatically.");
        }

        if (options.QuarantineCompetingFiles)
        {
            operations.AddRange(game.CompetingFiles.Select(path => new PatchOperation
            {
                Kind = PatchOperationKind.RemoveFile,
                TargetPath = path,
                Description = $"Quarantine competing loader {Path.GetFileName(path)}"
            }));
        }

        return new PatchPlan { Game = game, Options = options, Operations = operations, Warnings = warnings };
    }

    private void AddOverlay(GameScanResult game, List<PatchOperation> operations, List<string> warnings)
    {
        if (Path.GetFileName(game.ExecutablePath).Equals("Phasmophobia.exe", StringComparison.OrdinalIgnoreCase))
        {
            warnings.Add("Phasmophobia rejects an extra version.dll, so the optional overlay proxy will be skipped.");
            return;
        }

        string? source = artifacts.FindOverlayProxy();
        if (source is null)
        {
            warnings.Add("overlay_proxy.dll is not included in this build, so early overlay loading will be skipped.");
            return;
        }

        string? fileName = game.Engine switch
        {
            EngineKind.Unity => "version.dll",
            EngineKind.Unreal => "XINPUT1_3.dll",
            _ => null
        };
        if (fileName is null)
            return;

        operations.Add(new PatchOperation
        {
            Kind = PatchOperationKind.ReplaceFile,
            SourcePath = source,
            TargetPath = Path.Combine(Path.GetDirectoryName(game.ExecutablePath)!, fileName),
            Description = $"Install early overlay proxy as {fileName}"
        });
    }

    private void AddPlugin(string name, GameScanResult game, List<PatchOperation> operations)
    {
        string? source = artifacts.FindPlugin(name);
        if (source is null)
            throw new FileNotFoundException($"Selected plugin {name}.dll was not found beside the patcher.");
        operations.Add(new PatchOperation
        {
            Kind = PatchOperationKind.ReplaceFile,
            SourcePath = source,
            TargetPath = Path.Combine(game.ConfigDirectory, "plugins", name + ".dll"),
            Description = $"Install {name}.dll"
        });
    }
}
