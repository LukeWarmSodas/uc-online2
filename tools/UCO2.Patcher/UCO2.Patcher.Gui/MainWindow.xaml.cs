using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows;
using Microsoft.Win32;
using UCO2.Patcher.Core;

namespace UCO2.Patcher.Gui;

public partial class MainWindow : Window
{
    private readonly SteamStoreSearchService steamStore = new();
    private readonly GameScanner scanner = new();
    private readonly ArtifactLocator artifacts;
    private readonly BackupService backups = new();
    private readonly InstalledFixService installedFixes = new();
    private readonly FixPackager packager = new();
    private readonly string? initialGameDirectory;
    private bool refreshFixAfterUpdate;
    private bool loadingSteamMatches;
    private GameScanResult? scan;
    private PatchPlan? plan;

    public MainWindow(string? initialGameDirectory = null, bool refreshFixAfterUpdate = false)
    {
        InitializeComponent();
        this.initialGameDirectory = initialGameDirectory;
        this.refreshFixAfterUpdate = refreshFixAfterUpdate;
        artifacts = new ArtifactLocator(FindArtifactRoot());
        VersionText.Text = $"Version {artifacts.CurrentVersion}  |  {(IsAdministrator() ? "Administrator" : "Standard user")}";
        Loaded += MainWindow_Loaded;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        if (!string.IsNullOrWhiteSpace(initialGameDirectory) && Directory.Exists(initialGameDirectory))
            await ScanGameAsync(initialGameDirectory);
    }

    private async Task ScanGameAsync(string directory)
    {
        SetBusy(true, "Scanning game files...");
        try
        {
            scan = await scanner.ScanAsync(directory);
            string folderName = Path.GetFileName(Path.TrimEndingDirectorySeparator(directory));
            SelectedTitle.Text = folderName;
            SelectedPath.Text = scan.GameDirectory;
            FolderPathBox.Text = scan.GameDirectory;
            FolderHintText.Text = "Scan complete";
            EngineValue.Text = scan.EngineLabel;
            ExecutableValue.Text = scan.ExecutablePath;
            SteamApiValue.Text = scan.SteamApiPath;
            ConfigValue.Text = scan.ConfigDirectory;
            ServicesValue.Text = scan.BackendLabel;
            SteamStubValue.Text = scan.SteamStub.ToString();
            WarningList.ItemsSource = scan.Warnings;

            LoadConfiguration(scan, null);
            await SearchSteamAsync(scan.GameDirectory);
            await RefreshBackupsAsync();
            RefreshPlan(selectChangesTab: false);
            ApplyButton.IsEnabled = true;
            Log($"Scanned {scan.GameDirectory}: {scan.EngineLabel}, {scan.BackendLabel}.");
            StatusText.Text = "Preflight complete";

            // EOS needs the user's own Epic app credentials -- surface them up front.
            UpdateEosPrompt();
            if (EosCheck.IsChecked == true && EosCredentialsMissing())
            {
                AdvancedExpander.IsExpanded = true;
                StatusText.Text = "EOS detected - enter your Epic app credentials under Advanced settings.";
                Log("EOS backend detected. Enter your Epic app ProductId / SandboxId / DeploymentId / ClientId / ClientSecret in Advanced settings before applying.");
            }

            if (refreshFixAfterUpdate)
            {
                refreshFixAfterUpdate = false;
                MessageBoxResult answer = MessageBox.Show(
                    "The patcher was updated and its bundled DLLs may be newer. Back up and refresh this installed fix now?",
                    "Update installed fix", MessageBoxButton.YesNo, MessageBoxImage.Question);
                if (answer == MessageBoxResult.Yes)
                    await ApplyPlanAsync("Update installed fix");
            }
        }
        catch (Exception ex)
        {
            scan = null;
            plan = null;
            ApplyButton.IsEnabled = false;
            UpdateFixButton.IsEnabled = false;
            PackageButton.IsEnabled = false;
            ShowError("Game scan failed", ex);
        }
        finally { SetBusy(false); }
    }

    private async Task SearchSteamAsync(string gameDirectory)
    {
        string existingAppId = OriginalAppIdBox.Text.Trim();
        string term = SteamStoreSearchService.BuildSearchTerm(gameDirectory);
        SteamSearchStatus.Text = $"Searching Steam for '{term}'...";
        try
        {
            IReadOnlyList<SteamSearchResult> results = await steamStore.SearchAsync(gameDirectory);
            loadingSteamMatches = true;
            SteamMatchBox.ItemsSource = results;
            SteamSearchResult? match = uint.TryParse(existingAppId, out uint configuredAppId)
                ? results.FirstOrDefault(result => result.AppId == configuredAppId)
                : null;
            match ??= SteamStoreSearchService.FindBestMatch(gameDirectory, results);
            SteamMatchBox.SelectedItem = match;
            if (match is not null)
            {
                if (!uint.TryParse(existingAppId, out _))
                    OriginalAppIdBox.Text = match.AppId.ToString();
                SelectedTitle.Text = match.Name;
                SteamSearchStatus.Text = $"{results.Count} match(es) found; verify the selected AppId";
                Log($"Steam Store matched '{term}' to {match.Name} ({match.AppId}).");
            }
            else
            {
                SteamSearchStatus.Text = "No Steam match found; enter the AppId manually";
                Log($"Steam Store returned no matches for '{term}'.");
            }
        }
        catch (Exception ex)
        {
            SteamMatchBox.ItemsSource = null;
            SteamSearchStatus.Text = "Steam search unavailable; enter the AppId manually";
            Log($"Steam search failed: {ex.Message}");
        }
        finally
        {
            loadingSteamMatches = false;
        }
    }

    private void LoadConfiguration(GameScanResult game, uint? steamAppId)
    {
        string iniPath = Path.Combine(game.ConfigDirectory, "union-crax.ini");
        IniDocument ini = IniDocument.Load(iniPath);
        AppIdBox.Text = ini.Get("Settings", "AppId", "480");
        OriginalAppIdBox.Text = ini.Get("Settings", "ogAppId", steamAppId?.ToString() ?? "");
        OverlayCheck.IsChecked = game.Architecture == GameArchitecture.X64;
        QuarantineCheck.IsChecked = true;
        UnlockDlcCheck.IsChecked = ini.GetBool("DLC", "UnlockAll", true);
        SteamStubCheck.IsChecked = ini.GetBool("Settings", "GetStubbedLol", game.SteamStub == SteamStubStatus.Detected);
        LoadOverlayCheck.IsChecked = ini.GetBool("Settings", "LoadOverlay", true);
        LogOverlayCheck.IsChecked = ini.GetBool("Settings", "LogOverlay");
        WarnOverlayCheck.IsChecked = ini.GetBool("Settings", "WarnOverlayDisabled");
        VerboseLogCheck.IsChecked = ini.GetBool("Settings", "VerboseLog");
        ForceOwnershipCheck.IsChecked = ini.GetBool("Settings", "ForceOwnership", true);
        PassthroughTicketCheck.IsChecked = ini.GetBool("Settings", "PassthroughTicket");
        EmulateTicketCheck.IsChecked = ini.GetBool("Settings", "EmulateTicket");
        SdrCheck.IsChecked = ini.GetBool("Settings", "SDR");
        LegacyClientBox.Text = ini.Get("Settings", "Client");

        PhotonCheck.IsChecked = game.Backends.HasFlag(BackendKind.PhotonRealtime) || game.Backends.HasFlag(BackendKind.PhotonFusion)
            || File.Exists(Path.Combine(game.ConfigDirectory, "plugins", "photon_universal.dll"));
        EosCheck.IsChecked = game.Backends.HasFlag(BackendKind.Eos)
            || File.Exists(Path.Combine(game.ConfigDirectory, "plugins", "EOS_custom.dll"));
        PlayFabCheck.IsChecked = game.Backends.HasFlag(BackendKind.PlayFab)
            || File.Exists(Path.Combine(game.ConfigDirectory, "plugins", "playfab_universal.dll"));
        CoherenceCheck.IsChecked = game.Backends.HasFlag(BackendKind.Coherence)
            || File.Exists(Path.Combine(game.ConfigDirectory, "plugins", "coherence_universal.dll"));

        PhotonRealtimeBox.Text = ini.Get("Realtime", "PhotonAppIdRealtime");
        PhotonFusionBox.Text = ini.Get("Fusion", "PhotonAppIdFusion");
        PhotonVoiceBox.Text = ini.Get("Realtime", "PhotonAppIdVoice", ini.Get("Fusion", "PhotonAppIdVoice"));
        EosProductBox.Text = ini.Get("EOS", "ProductId");
        EosSandboxBox.Text = ini.Get("EOS", "SandboxId");
        EosDeploymentBox.Text = ini.Get("EOS", "DeploymentId");
        EosClientBox.Text = ini.Get("EOS", "ClientId");
        EosSecretBox.Password = ini.Get("EOS", "ClientSecret");
        DisplayNameBox.Text = ini.Get("EOS", "DisplayName", "Player");
        PlayFabTitleBox.Text = ini.Get("PlayFab", "TitleId");
        CoherenceKeyBox.Text = ini.Get("Coherence", "RuntimeKey");

        string[] known =
        [
            "AppId", "ogAppId", "PluginsFolder", "GetStubbedLol", "LoadOverlay", "LogOverlay",
            "WarnOverlayDisabled", "VerboseLog", "ForceOwnership", "PassthroughTicket", "EmulateTicket",
            "SDR", "Client"
        ];
        AdditionalFlagsBox.Text = string.Join(Environment.NewLine, ini.GetSection("Settings")
            .Where(pair => !known.Contains(pair.Key, StringComparer.OrdinalIgnoreCase))
            .Select(pair => $"{pair.Key}={pair.Value}"));
    }

    private PatchOptions ReadOptions()
    {
        if (!uint.TryParse(AppIdBox.Text.Trim(), out uint appId) || appId == 0)
            throw new InvalidOperationException("Steam AppId must be a positive number.");
        if (!uint.TryParse(OriginalAppIdBox.Text.Trim(), out uint originalAppId) || originalAppId == 0)
            throw new InvalidOperationException("Enter the game's real Steam AppId.");

        var options = new PatchOptions
        {
            AppId = appId,
            OriginalAppId = originalAppId,
            UnlockAllDlc = UnlockDlcCheck.IsChecked == true,
            InstallOverlayProxy = OverlayCheck.IsChecked == true,
            QuarantineCompetingFiles = QuarantineCheck.IsChecked == true,
            EnableSteamStub = SteamStubCheck.IsChecked == true,
            LoadOverlay = LoadOverlayCheck.IsChecked == true,
            LogOverlay = LogOverlayCheck.IsChecked == true,
            WarnOverlayDisabled = WarnOverlayCheck.IsChecked == true,
            VerboseLog = VerboseLogCheck.IsChecked == true,
            ForceOwnership = ForceOwnershipCheck.IsChecked == true,
            PassthroughTicket = PassthroughTicketCheck.IsChecked == true,
            EmulateTicket = EmulateTicketCheck.IsChecked == true,
            EnableSdr = SdrCheck.IsChecked == true,
            InstallPhoton = PhotonCheck.IsChecked == true,
            InstallEos = EosCheck.IsChecked == true,
            InstallPlayFab = PlayFabCheck.IsChecked == true,
            InstallCoherence = CoherenceCheck.IsChecked == true,
            PhotonRealtimeAppId = PhotonRealtimeBox.Text,
            PhotonFusionAppId = PhotonFusionBox.Text,
            PhotonVoiceAppId = PhotonVoiceBox.Text,
            EosProductId = EosProductBox.Text,
            EosSandboxId = EosSandboxBox.Text,
            EosDeploymentId = EosDeploymentBox.Text,
            EosClientId = EosClientBox.Text,
            EosClientSecret = EosSecretBox.Password,
            DisplayName = DisplayNameBox.Text,
            PlayFabTitleId = PlayFabTitleBox.Text,
            CoherenceRuntimeKey = CoherenceKeyBox.Text,
            LegacyClientVersion = LegacyClientBox.Text
        };

        string[] reserved =
        [
            "AppId", "ogAppId", "PluginsFolder", "GetStubbedLol", "LoadOverlay", "LogOverlay",
            "WarnOverlayDisabled", "VerboseLog", "ForceOwnership", "PassthroughTicket", "EmulateTicket",
            "SDR", "Client"
        ];
        foreach (string raw in AdditionalFlagsBox.Text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            int separator = raw.IndexOf('=');
            if (separator <= 0) throw new InvalidOperationException($"Additional flag must use key=value: {raw}");
            string key = raw[..separator].Trim();
            if (reserved.Contains(key, StringComparer.OrdinalIgnoreCase))
                throw new InvalidOperationException($"{key} already has a dedicated control and cannot be duplicated in Additional flags.");
            options.AdditionalSettings[key] = raw[(separator + 1)..].Trim();
        }
        return options;
    }

    private void RefreshPlan(bool selectChangesTab)
    {
        if (scan is null) return;
        plan = new PatchPlanner(artifacts).Create(scan, ReadOptions());
        OperationList.ItemsSource = plan.Operations;
        WarningList.ItemsSource = plan.Warnings;
        IReadOnlyList<InstalledFileStatus> statuses = installedFixes.Inspect(plan);
        int stale = statuses.Count(status => !status.Current);
        UpdateFixButton.IsEnabled = stale > 0;
        PackageButton.IsEnabled = statuses.Any(status => status.Exists);
        StatusText.Text = stale == 0 ? "Installed fix is current" : $"{stale} installed file(s) need an update";
        if (selectChangesTab) MainTabs.SelectedIndex = 0; // Changes is the first details tab now
    }

    // The Changes/Backups/Activity strip stays hidden until the user reviews.
    private void RevealDetails(int tab = 0)
    {
        MainTabs.Visibility = Visibility.Visible;
        MainTabs.SelectedIndex = tab;
    }

    // EOS_custom needs the user's OWN Epic app; without these it must not deploy.
    private bool EosCredentialsMissing() =>
        string.IsNullOrWhiteSpace(EosProductBox.Text)
        || string.IsNullOrWhiteSpace(EosSandboxBox.Text)
        || string.IsNullOrWhiteSpace(EosDeploymentBox.Text)
        || string.IsNullOrWhiteSpace(EosClientBox.Text)
        || string.IsNullOrWhiteSpace(EosSecretBox.Password);

    private void EosField_Changed(object sender, System.Windows.Controls.TextChangedEventArgs e) => UpdateEosPrompt();
    private void EosSecret_Changed(object sender, RoutedEventArgs e) => UpdateEosPrompt();
    private void EosCheck_Toggled(object sender, RoutedEventArgs e)
    {
        UpdateEosPrompt();
        if (EosCheck.IsChecked == true && EosCredentialsMissing())
            AdvancedExpander.IsExpanded = true; // surface the required group
    }

    // Paint the EOS group yellow + show the REQUIRED badge while its credentials
    // are needed; clear it live as the user fills them in.
    private void UpdateEosPrompt()
    {
        if (EosCredsGroup is null) return; // fires during InitializeComponent
        bool needed = EosCheck.IsChecked == true && EosCredentialsMissing();
        EosRequiredBadge.Visibility = needed ? Visibility.Visible : Visibility.Collapsed;
        EosCredsGroup.BorderBrush = (System.Windows.Media.Brush)FindResource(needed ? "WarnBrush" : "BorderBrush");
        EosCredsGroup.Background = (System.Windows.Media.Brush)FindResource(needed ? "WarnBgBrush" : "PanelAltBrush");
    }

    private async Task ApplyPlanAsync(string actionName)
    {
        if (scan is null) return;
        RefreshPlan(selectChangesTab: false);
        if (plan is null) return;

        if (EosCheck.IsChecked == true && EosCredentialsMissing())
        {
            AdvancedExpander.IsExpanded = true;
            MessageBox.Show(
                "EOS is enabled but your Epic app credentials are missing.\n\n" +
                "Enter ProductId, SandboxId, DeploymentId, ClientId and ClientSecret under " +
                "Advanced settings — or turn EOS off.",
                "EOS credentials required", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var review = new ReviewWindow(plan, SelectedTitle.Text, actionName) { Owner = this };
        if (review.ShowDialog() != true) return;

        RevealDetails(2); // show the Activity log while patching runs
        SetBusy(true, actionName + "...");
        try
        {
            var progress = new Progress<string>(message => { StatusText.Text = message; Log(message); });
            BackupManifest manifest = await backups.ApplyAsync(plan, progress);
            Log($"Patch complete. Snapshot {manifest.Id} contains {manifest.Entries.Count} operation(s).");
            await RefreshBackupsAsync();
            RefreshPlan(selectChangesTab: false);
            StatusText.Text = "Patch completed and verified";
            MessageBox.Show("The fix was backed up, installed, and verified.", "UCOnline2", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            ShowError("Patch failed", ex);
            if (ContainsUnauthorized(ex) && !IsAdministrator())
            {
                if (MessageBox.Show("This game folder requires administrator access. Restart the patcher elevated?", "Administrator access required", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
                    RestartElevated();
            }
        }
        finally { SetBusy(false); }
    }

    private async Task RefreshBackupsAsync()
    {
        if (scan is null) return;
        IReadOnlyList<BackupSnapshot> snapshots = await backups.ListAsync(scan.GameDirectory);
        BackupGrid.ItemsSource = snapshots;
        BackupGrid.SelectedItem = snapshots.FirstOrDefault(snapshot => snapshot.Status == "Complete");
        PackageButton.IsEnabled = PackageButton.IsEnabled || snapshots.Any(snapshot => snapshot.Status == "Complete");
    }

    private async void Apply_Click(object sender, RoutedEventArgs e) => await ApplyPlanAsync("Back up and patch");
    private async void UpdateFix_Click(object sender, RoutedEventArgs e) => await ApplyPlanAsync("Update installed fix");

    private void Preview_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            RefreshPlan(selectChangesTab: false);
            if (plan is not null) RevealDetails(0); // reveal the Changes strip inline
        }
        catch (Exception ex) { ShowError("Cannot build patch plan", ex); }
    }

    private async void Restore_Click(object sender, RoutedEventArgs e)
    {
        if (BackupGrid.SelectedItem is not BackupSnapshot snapshot) return;
        if (MessageBox.Show($"Restore snapshot {snapshot.Id}? Current files at those paths will be replaced.", "Restore backup", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        SetBusy(true, "Restoring backup...");
        try
        {
            await backups.RestoreAsync(snapshot.ManifestPath, new Progress<string>(message => { StatusText.Text = message; Log(message); }));
            await RefreshBackupsAsync();
            RefreshPlan(selectChangesTab: false);
            MessageBox.Show("Backup restored and verified.", "UCOnline2", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex) { ShowError("Restore failed", ex); }
        finally { SetBusy(false); }
    }

    private async void PackageFix_Click(object sender, RoutedEventArgs e)
    {
        if (plan is null) return;
        if (MessageBox.Show("The package contains the deployed union-crax.ini, including any backend credentials in it. Create a shareable ZIP in the game root?", "Package current fix", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        SetBusy(true, "Packaging current fix...");
        try
        {
            FixPackageResult result = await packager.CreateAsync(plan, new Progress<string>(message => { StatusText.Text = message; Log(message); }));
            Log($"Created {result.ArchivePath} with {result.FileCount} file(s).");
            MessageBox.Show($"Created {Path.GetFileName(result.ArchivePath)} in the game root.", "Fix package ready", MessageBoxButton.OK, MessageBoxImage.Information);
            Process.Start(new ProcessStartInfo { FileName = "explorer.exe", Arguments = $"/select,\"{result.ArchivePath}\"", UseShellExecute = true });
        }
        catch (Exception ex) { ShowError("Fix packaging failed", ex); }
        finally { SetBusy(false); }
    }

    private async void CheckUpdates_Click(object sender, RoutedEventArgs e)
    {
        SetBusy(true, "Checking GitHub releases...");
        try
        {
            using var updates = new UpdateService();
            ReleaseInfo release = await updates.GetLatestAsync();
            if (!UpdateService.IsNewer(artifacts.CurrentVersion, release.TagName))
            {
                MessageBox.Show(artifacts.CurrentVersion == "development" ? $"Development build. Latest regular release: {release.TagName}" : $"You already have {artifacts.CurrentVersion}.", "UCOnline2 updates", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }
            if (release.ReleaseArchive is null) throw new InvalidOperationException("The latest release has no release ZIP.");
            if (MessageBox.Show($"Download and install {release.TagName}? The selected game can be refreshed with the new DLLs after restart.", "UCOnline2 update available", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
            string archive = await updates.DownloadAsync(release.ReleaseArchive, new Progress<double>(value => Progress.Value = value));
            string executable = Environment.ProcessPath ?? throw new InvalidOperationException("Executable path unavailable.");
            Process.Start(SelfUpdateService.CreateUpdaterStartInfo(archive, AppContext.BaseDirectory, Path.GetFileName(executable), Environment.ProcessId, scan?.GameDirectory));
            Application.Current.Shutdown();
        }
        catch (Exception ex) { ShowError("Update failed", ex); }
        finally { SetBusy(false); }
    }

    private void OpenBackupFolder_Click(object sender, RoutedEventArgs e)
    {
        if (BackupGrid.SelectedItem is not BackupSnapshot snapshot) return;
        Process.Start(new ProcessStartInfo { FileName = Path.GetDirectoryName(snapshot.ManifestPath)!, UseShellExecute = true });
    }

    private async void ScanAgain_Click(object sender, RoutedEventArgs e) { if (scan is not null) await ScanGameAsync(scan.GameDirectory); }

    private void OptionsScroller_Loaded(object sender, RoutedEventArgs e) => OptionsScroller.ScrollToTop();

    private async void BrowseFolder_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Choose the game folder", Multiselect = false };
        if (dialog.ShowDialog(this) == true)
            await ScanGameAsync(dialog.FolderName);
    }

    private void SteamMatchBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (loadingSteamMatches || SteamMatchBox.SelectedItem is not SteamSearchResult match) return;
        OriginalAppIdBox.Text = match.AppId.ToString();
        SelectedTitle.Text = match.Name;
        SteamSearchStatus.Text = $"Selected {match.Name} ({match.AppId})";
        try { RefreshPlan(selectChangesTab: false); }
        catch (Exception ex) { ShowError("Cannot update AppId", ex); }
    }

    private void RunElevated_Click(object sender, RoutedEventArgs e) => RestartElevated();

    private void RestartElevated()
    {
        string executable = Environment.ProcessPath ?? throw new InvalidOperationException("Executable path unavailable.");
        var start = new ProcessStartInfo { FileName = executable, UseShellExecute = true, Verb = "runas" };
        if (scan is not null) start.Arguments = $"--game \"{scan.GameDirectory}\"";
        try { Process.Start(start); Application.Current.Shutdown(); }
        catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223) { }
    }

    private void SetBusy(bool busy, string? status = null)
    {
        Progress.IsIndeterminate = busy;
        if (!busy) Progress.Value = 0;
        ApplyButton.IsEnabled = !busy && scan is not null;
        if (status is not null) StatusText.Text = status;
    }

    private void Log(string message)
    {
        ActivityLog.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        ActivityLog.ScrollToEnd();
    }

    private void ShowError(string title, Exception exception)
    {
        Log($"ERROR: {exception.Message}");
        StatusText.Text = exception.Message;
        MessageBox.Show(exception.Message, title, MessageBoxButton.OK, MessageBoxImage.Error);
    }

    private static bool ContainsUnauthorized(Exception exception)
    {
        for (Exception? current = exception; current is not null; current = current.InnerException)
            if (current is UnauthorizedAccessException) return true;
        return false;
    }

    private static bool IsAdministrator()
    {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static string FindArtifactRoot()
    {
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "uc_online2.vcxproj")) || Directory.Exists(Path.Combine(directory.FullName, "x64")))
                return directory.FullName;
            directory = directory.Parent;
        }
        return AppContext.BaseDirectory;
    }

    protected override void OnClosed(EventArgs e)
    {
        steamStore.Dispose();
        base.OnClosed(e);
    }

}
