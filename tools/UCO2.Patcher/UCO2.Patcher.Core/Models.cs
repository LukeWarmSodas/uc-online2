using System.Text.Json.Serialization;

namespace UCO2.Patcher.Core;

public enum EngineKind
{
    Unknown,
    Unity,
    Unreal,
    Generic
}

public enum GameArchitecture
{
    Unknown,
    X86,
    X64
}

public enum SteamStubStatus
{
    NotDetected,
    Detected,
    UnrecognizedBindSection,
    Unreadable
}

[Flags]
public enum BackendKind
{
    None = 0,
    PhotonRealtime = 1,
    PhotonFusion = 2,
    PhotonVoice = 4,
    Eos = 8,
    PlayFab = 16,
    Coherence = 32
}

public sealed record SteamGame(
    uint AppId,
    string Name,
    string InstallDirectory,
    string ManifestPath,
    string LibraryPath);

public sealed record SteamSearchResult(
    uint AppId,
    string Name,
    string? ImageUrl)
{
    public override string ToString() => $"{Name}  ({AppId})";
}

public sealed class GameScanResult
{
    public required string GameDirectory { get; init; }
    public required EngineKind Engine { get; init; }
    public required GameArchitecture Architecture { get; init; }
    public required string SteamApiPath { get; init; }
    // Every steam_api64.dll copy in the game folder. Unity games sometimes ship
    // one in the root AND one under Data/Plugins/x86_64; the engine loads the
    // Data/Plugins copy, so we replace them ALL rather than guess which is live.
    public IReadOnlyList<string> SteamApiPaths { get; init; } = [];
    public required string ExecutablePath { get; init; }
    public required string ConfigDirectory { get; init; }
    public string? UnityDataDirectory { get; init; }
    public BackendKind Backends { get; init; }
    public SteamStubStatus SteamStub { get; init; }
    public IReadOnlyList<string> CompetingFiles { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];

    [JsonIgnore]
    public string EngineLabel => Architecture == GameArchitecture.Unknown
        ? Engine.ToString()
        : $"{Engine} / {Architecture}";

    [JsonIgnore]
    public string BackendLabel => Backends == BackendKind.None
        ? "Steam only"
        : string.Join(", ", Enum.GetValues<BackendKind>()
            .Where(value => value != BackendKind.None && Backends.HasFlag(value))
            .Select(value => value switch
            {
                BackendKind.PhotonRealtime => "Photon Realtime",
                BackendKind.PhotonFusion => "Photon Fusion",
                BackendKind.PhotonVoice => "Photon Voice",
                BackendKind.Eos => "EOS",
                BackendKind.PlayFab => "PlayFab",
                BackendKind.Coherence => "coherence",
                _ => value.ToString()
            }));
}

public sealed class PatchOptions
{
    public uint AppId { get; set; } = 480;
    public uint OriginalAppId { get; set; }
    public bool UnlockAllDlc { get; set; } = true;
    public bool InstallOverlayProxy { get; set; } = true;
    public bool QuarantineCompetingFiles { get; set; } = true;
    public bool EnableSteamStub { get; set; }
    public bool LoadOverlay { get; set; } = true;
    public bool LogOverlay { get; set; }
    public bool WarnOverlayDisabled { get; set; }
    public bool VerboseLog { get; set; }
    public bool ForceOwnership { get; set; } = true;
    public bool PassthroughTicket { get; set; }
    public bool EmulateTicket { get; set; }
    public bool EnableSdr { get; set; }
    public bool InventoryAutoGrant { get; set; }
    public bool InstallPhoton { get; set; }
    public bool InstallEos { get; set; }
    public bool InstallPlayFab { get; set; }
    public bool InstallCoherence { get; set; }
    public string PhotonRealtimeAppId { get; set; } = "";
    public string PhotonFusionAppId { get; set; } = "";
    public string PhotonVoiceAppId { get; set; } = "";
    public string EosProductId { get; set; } = "";
    public string EosSandboxId { get; set; } = "";
    public string EosDeploymentId { get; set; } = "";
    public string EosClientId { get; set; } = "";
    public string EosClientSecret { get; set; } = "";
    public string DisplayName { get; set; } = "Player";
    // Device ID login on the game's OWN Epic app (no redirect); wins over the ids above.
    public bool EosKeepGameApp { get; set; }
    public string PlayFabTitleId { get; set; } = "";
    // Anonymous login on the game's OWN PlayFab title (no redirect); wins over TitleId.
    public bool PlayFabKeepGameTitle { get; set; }
    public string CoherenceRuntimeKey { get; set; } = "";
    public string LegacyClientVersion { get; set; } = "";
    public Dictionary<string, string> AdditionalSettings { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

public enum PatchOperationKind
{
    ReplaceFile,
    WriteText,
    WriteBytes,
    RemoveFile
}

public sealed class PatchOperation
{
    public required PatchOperationKind Kind { get; init; }
    public required string TargetPath { get; init; }
    public string? SourcePath { get; init; }
    public string? TextContent { get; init; }
    [JsonIgnore]
    public byte[]? BinaryContent { get; init; }
    public required string Description { get; init; }
}

public sealed class PatchPlan
{
    public required GameScanResult Game { get; init; }
    public required PatchOptions Options { get; init; }
    public required IReadOnlyList<PatchOperation> Operations { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class BackupManifest
{
    public int FormatVersion { get; set; } = 1;
    public required string Id { get; set; }
    public required string GameDirectory { get; set; }
    public required DateTimeOffset CreatedAt { get; set; }
    public string Status { get; set; } = "InProgress";
    public string? Error { get; set; }
    public List<BackupEntry> Entries { get; set; } = [];
}

public sealed class BackupEntry
{
    public required PatchOperationKind Kind { get; set; }
    public required string TargetPath { get; set; }
    public required string Description { get; set; }
    public bool Existed { get; set; }
    public string? OriginalHash { get; set; }
    public string? BackupRelativePath { get; set; }
    public string? ReplacementHash { get; set; }
    public bool Applied { get; set; }
}

public sealed record BackupSnapshot(
    string ManifestPath,
    string Id,
    DateTimeOffset CreatedAt,
    string Status,
    int FileCount);

public sealed record ReleaseAsset(
    string Name,
    string DownloadUrl,
    string? Digest,
    long Size);

public sealed record ReleaseInfo(
    string TagName,
    string Name,
    string HtmlUrl,
    bool IsPrerelease,
    ReleaseAsset? ReleaseArchive);

public sealed record InstalledFileStatus(string Description, string TargetPath, bool Exists, bool Current);

public sealed record FixPackageResult(string ArchivePath, int FileCount);
