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
