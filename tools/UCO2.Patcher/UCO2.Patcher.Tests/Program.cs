using System.IO.Compression;
using System.Text;
using UCO2.Patcher.Core;

namespace UCO2.Patcher.Tests;

internal static class Program
{
    private static int passed;

    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 2 && args[0].Equals("--scan", StringComparison.OrdinalIgnoreCase))
        {
            GameScanResult result = await new GameScanner().ScanAsync(args[1]);
            Console.WriteLine($"{result.EngineLabel}|{result.ExecutablePath}|{result.SteamApiPath}|{result.BackendLabel}");
            return 0;
        }

        string root = Path.Combine(Path.GetTempPath(), "UCO2.Patcher.Tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            TestSteamVdfParsing();
            TestSteamStoreSearch();
            TestConfigFlags(root);
            TestBackendProfiles(root);
            TestBackendValidation(root);
            TestPlayFabPlanning(root);
            await TestSelfUpdateLayout(root);
            await TestBackupRestoreAndPackage(root);
            Console.WriteLine($"PASS: {passed} tests");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("FAIL: " + ex);
            return 1;
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); } catch { }
        }
    }

    private static void TestSteamStoreSearch()
    {
        Equal("Phasmophobia", SteamStoreSearchService.BuildSearchTerm(@"C:\Games\Phasmophobia.Build.24434979"), "Release suffix cleanup");
        const string json = """
            {"total":2,"items":[
              {"type":"app","name":"Schedule I","id":3164500,"tiny_image":"https://example.invalid/one.jpg"},
              {"type":"app","name":"Schedule I: Free Sample","id":3205720}
            ]}
            """;
        using var stream = new MemoryStream(Encoding.UTF8.GetBytes(json));
        IReadOnlyList<SteamSearchResult> results = SteamStoreSearchService.ParseResults(stream);
        Equal(2, results.Count, "Steam result count");
        Equal((uint)3164500, results[0].AppId, "Steam result AppId");
        Equal((uint)3164500, SteamStoreSearchService.FindBestMatch(@"C:\Games\Schedule.I", results)!.AppId, "Exact Steam match");

        const string detailsJson = """
            {"3164500":{"success":true,"data":{"type":"game","name":"Schedule I","header_image":"https://example.invalid/header.jpg"}}}
            """;
        using var detailsStream = new MemoryStream(Encoding.UTF8.GetBytes(detailsJson));
        SteamSearchResult? details = SteamStoreSearchService.ParseAppDetails(detailsStream, 3164500);
        Equal("Schedule I", details!.Name, "Manual AppId name");
        Equal((uint)3164500, details.AppId, "Manual AppId result");
        passed++;
    }

    private static void TestSteamVdfParsing()
    {
        const string vdf = "\"libraryfolders\"\n{\n \"0\" { \"path\" \"C:\\\\Program Files (x86)\\\\Steam\" }\n \"1\" { \"path\" \"D:\\\\Steam Library\" }\n}";
        IReadOnlyList<string> paths = SteamLibraryService.ParseLibraryFolders(vdf);
        Equal(2, paths.Count, "VDF library count");
        True(paths.Contains(@"D:\Steam Library", StringComparer.OrdinalIgnoreCase), "VDF escaped path");

        Dictionary<string, string> app = SteamLibraryService.ParseFlatVdf("\"appid\" \"480\"\n\"name\" \"Spacewar\"\n\"installdir\" \"Spacewar\"");
        Equal("Spacewar", app["name"], "Manifest name");
        passed++;
    }

    private static void TestConfigFlags(string root)
    {
        GameScanResult game = FakeGame(root);
        var options = new PatchOptions
        {
            OriginalAppId = 123,
            InstallOverlayProxy = false,
            EnableSteamStub = true,
            LoadOverlay = false,
            PassthroughTicket = true,
            LegacyClientVersion = "017",
            VerboseLog = true
        };
        options.AdditionalSettings["CustomFlag"] = "yes";
        string config = ConfigBuilder.Build(game, options);
        True(config.Contains("GetStubbedLol=true"), "SteamStub flag");
        True(config.Contains("LoadOverlay=false"), "Overlay flag");
        True(config.Contains("PassthroughTicket=true"), "Passthrough flag");
        True(config.Contains("Client=017"), "Client flag");
        True(config.Contains("CustomFlag=yes"), "Custom flag");
        passed++;
    }

    private static void TestBackendProfiles(string root)
    {
        string path = Path.Combine(root, "backend-profiles.dat");
        var store = new BackendProfileStore(path);
        var options = new PatchOptions
        {
            OriginalAppId = 123,
            InstallPhoton = true,
            PhotonRealtimeAppId = "photon-realtime",
            PhotonVoiceAppId = "photon-voice",
            InstallEos = true,
            EosProductId = "eos-product",
            EosSandboxId = "eos-sandbox",
            EosDeploymentId = "eos-deployment",
            EosClientId = "eos-client",
            EosClientSecret = "super-secret",
            DisplayName = "Player One",
            InstallPlayFab = true,
            PlayFabTitleId = "ABCDE"
        };

        IReadOnlyList<string> saved = store.SaveFrom(options);
        Equal(3, saved.Count, "Saved backend profile count");
        True(!Encoding.UTF8.GetString(File.ReadAllBytes(path)).Contains("super-secret", StringComparison.Ordinal), "Backend profile encrypted at rest");

        BackendProfiles profiles = store.Load(out string? warning);
        True(warning is null, "Backend profile decrypt warning");
        Equal("photon-realtime", profiles.Photon!.RealtimeAppId, "Remembered Photon AppId");
        Equal("super-secret", profiles.Eos!.ClientSecret, "Remembered EOS secret");
        Equal("ABCDE", profiles.PlayFab!.TitleId, "Remembered PlayFab TitleId");
        passed++;
    }

    private static void TestBackendValidation(string root)
    {
        GameScanResult game = FakeGame(root);
        var selected = new PatchOptions
        {
            InstallPhoton = true,
            InstallEos = true,
            InstallPlayFab = true
        };
        IReadOnlyList<MissingBackendSettings> missing = BackendSettingsValidator.FindMissing(game, selected);
        Equal(3, missing.Count, "All selected backends require settings");
        True(missing.Any(item => item.Backend == "Photon"), "Photon requirement");
        True(missing.Any(item => item.Backend == "EOS"), "EOS requirement");
        True(missing.Any(item => item.Backend == "PlayFab"), "PlayFab requirement");

        var disabled = new PatchOptions();
        Equal(0, BackendSettingsValidator.FindMissing(game, disabled).Count, "Disabled backends require no settings");
        passed++;
    }

    private static void TestPlayFabPlanning(string root)
    {
        string artifactRoot = Path.Combine(root, "PlannerArtifacts");
        string gameRoot = Path.Combine(root, "PlannerGame");
        Directory.CreateDirectory(Path.Combine(artifactRoot, "x64"));
        Directory.CreateDirectory(Path.Combine(artifactRoot, "plugins"));
        Directory.CreateDirectory(gameRoot);
        File.WriteAllBytes(Path.Combine(artifactRoot, "x64", "steam_api64.dll"), [1]);
        File.WriteAllBytes(Path.Combine(artifactRoot, "plugins", "playfab_universal.dll"), [2]);

        var planner = new PatchPlanner(new ArtifactLocator(artifactRoot));
        var options = new PatchOptions
        {
            OriginalAppId = 123,
            InstallOverlayProxy = false,
            InstallPlayFab = true
        };

        PatchPlan incomplete = planner.Create(FakeGame(gameRoot), options);
        True(incomplete.Warnings.Any(warning => warning.Contains("TitleId is empty", StringComparison.Ordinal)), "Empty PlayFab TitleId warning");
        True(!incomplete.Operations.Any(operation => operation.Description.Contains("playfab_universal", StringComparison.Ordinal)), "Empty PlayFab TitleId skips plugin");

        options.PlayFabTitleId = "ABCDE";
        PatchPlan complete = planner.Create(FakeGame(gameRoot), options);
        True(complete.Operations.Any(operation => operation.Description.Contains("playfab_universal", StringComparison.Ordinal)), "Configured PlayFab plugin planned");
        True(!complete.Warnings.Any(warning => warning.Contains("TitleId", StringComparison.Ordinal)), "Configured PlayFab has no TitleId warning");
        passed++;
    }

    private static async Task TestSelfUpdateLayout(string root)
    {
        string sourceRoot = Path.Combine(root, "UpdateSource");
        string packageRoot = Path.Combine(sourceRoot, "uc-online2-v9.9.9-release");
        Directory.CreateDirectory(Path.Combine(packageRoot, "x64"));
        Directory.CreateDirectory(Path.Combine(packageRoot, "x86"));
        Directory.CreateDirectory(Path.Combine(packageRoot, "plugins"));
        File.WriteAllText(Path.Combine(packageRoot, "UCO2.Patcher.exe"), "new-patcher");
        File.WriteAllText(Path.Combine(packageRoot, "x64", "steam_api64.dll"), "new-x64");
        File.WriteAllText(Path.Combine(packageRoot, "x86", "steam_api.dll"), "new-x86");
        File.WriteAllText(Path.Combine(packageRoot, "plugins", "playfab_universal.dll"), "new-plugin");

        string archive = Path.Combine(root, "update package.zip");
        ZipFile.CreateFromDirectory(sourceRoot, archive);
        string executableDirectory = Path.Combine(root, "Nested Executable");
        string artifactDirectory = Path.Combine(root, "Artifact Root");
        Directory.CreateDirectory(executableDirectory);
        Directory.CreateDirectory(artifactDirectory);
        File.WriteAllText(Path.Combine(executableDirectory, "UCO2.Patcher.exe"), "old-patcher");

        string installedExecutable = await SelfUpdateService.InstallPackageAsync(
            archive, executableDirectory, artifactDirectory, "UCO2.Patcher.exe", "v9.9.9");

        Equal("new-patcher", File.ReadAllText(installedExecutable), "Updater replaces nested executable");
        Equal("new-x64", File.ReadAllText(Path.Combine(artifactDirectory, "x64", "steam_api64.dll")), "Updater replaces x64 Steam API");
        Equal("new-x86", File.ReadAllText(Path.Combine(artifactDirectory, "x86", "steam_api.dll")), "Updater replaces x86 Steam API");
        Equal("new-plugin", File.ReadAllText(Path.Combine(artifactDirectory, "plugins", "playfab_universal.dll")), "Updater replaces plugin DLLs");

        string gameWithSpaces = Path.Combine(root, "Game With Spaces");
        var startInfo = SelfUpdateService.CreateUpdaterStartInfo(
            archive, installedExecutable, artifactDirectory, 42, gameWithSpaces, "v9.9.9");
        Equal(8, startInfo.ArgumentList.Count, "Updater argument count");
        Equal(archive, startInfo.ArgumentList[1], "Updater archive path with spaces");
        Equal(artifactDirectory, startInfo.ArgumentList[3], "Updater artifact path with spaces");
        Equal(gameWithSpaces, startInfo.ArgumentList[6], "Updater game path with spaces");
        try { Directory.Delete(Path.GetDirectoryName(startInfo.FileName)!, recursive: true); } catch { }
        passed++;
    }

    private static async Task TestBackupRestoreAndPackage(string root)
    {
        string gameRoot = Path.Combine(root, "Game");
        string sourceRoot = Path.Combine(root, "Source");
        Directory.CreateDirectory(Path.Combine(gameRoot, "bin"));
        Directory.CreateDirectory(sourceRoot);
        string target = Path.Combine(gameRoot, "bin", "steam_api64.dll");
        string source = Path.Combine(sourceRoot, "steam_api64.dll");
        await File.WriteAllTextAsync(target, "original");
        await File.WriteAllTextAsync(source, "replacement");

        var plan = new PatchPlan
        {
            Game = FakeGame(gameRoot),
            Options = new PatchOptions { OriginalAppId = 123 },
            Operations =
            [
                new PatchOperation { Kind = PatchOperationKind.ReplaceFile, SourcePath = source, TargetPath = target, Description = "Replace Steam API" },
                new PatchOperation { Kind = PatchOperationKind.WriteText, TextContent = "[Settings]\nAppId=480\n", TargetPath = Path.Combine(gameRoot, "union-crax.ini"), Description = "Write config" }
            ]
        };

        string backupRoot = Path.Combine(root, "Backups");
        var backups = new BackupService(backupRoot);
        BackupManifest manifest = await backups.ApplyAsync(plan);
        Equal("replacement", await File.ReadAllTextAsync(target), "Replacement applied");
        True(File.Exists(Path.Combine(gameRoot, "union-crax.ini")), "Created file applied");

        BackupSnapshot snapshot = (await backups.ListAsync(gameRoot)).Single();
        var packager = new FixPackager();
        FixPackageResult package = await packager.CreateAsync(snapshot.ManifestPath);
        using (ZipArchive zip = ZipFile.OpenRead(package.ArchivePath))
        {
            True(zip.GetEntry("bin/steam_api64.dll") is not null, "Package preserves nested path");
            True(zip.GetEntry("union-crax.ini") is not null, "Package contains config");
            True(zip.GetEntry("uco2-fix-manifest.json") is not null, "Package manifest");
        }

        await backups.RestoreAsync(snapshot.ManifestPath);
        Equal("original", await File.ReadAllTextAsync(target), "Original restored");
        True(!File.Exists(Path.Combine(gameRoot, "union-crax.ini")), "Created file removed on restore");
        passed++;
    }

    private static GameScanResult FakeGame(string root) => new()
    {
        GameDirectory = root,
        Engine = EngineKind.Generic,
        Architecture = GameArchitecture.X64,
        SteamApiPath = Path.Combine(root, "bin", "steam_api64.dll"),
        ExecutablePath = Path.Combine(root, "bin", "game.exe"),
        ConfigDirectory = root,
        SteamStub = SteamStubStatus.NotDetected
    };

    private static void True(bool value, string name)
    {
        if (!value) throw new InvalidOperationException($"Assertion failed: {name}");
    }

    private static void Equal<T>(T expected, T actual, string name) where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new InvalidOperationException($"Assertion failed: {name}. Expected {expected}, got {actual}.");
    }
}
