using System.IO;
using System.Windows;
using UCO2.Patcher.Core;

namespace UCO2.Patcher.Gui;

public partial class ReviewWindow : Window
{
    public ReviewWindow(PatchPlan plan, string gameName, string actionName, bool allowConfirm = true)
    {
        InitializeComponent();
        HeadingText.Text = actionName;
        ConfirmButton.Visibility = allowConfirm ? Visibility.Visible : Visibility.Collapsed;
        CancelButton.Content = allowConfirm ? "Cancel" : "Close";
        FooterText.Text = allowConfirm ? "Existing files are backed up before any write" : $"{plan.Operations.Count} file change(s) planned";
        GameValue.Text = gameName;
        FolderValue.Text = plan.Game.GameDirectory;
        DetectedValue.Text = $"{plan.Game.EngineLabel} | {plan.Game.BackendLabel}";
        AppIdsValue.Text = $"Game {plan.Options.OriginalAppId} | Emulated {plan.Options.AppId}";

        SettingsList.ItemsSource = BuildSettings(plan.Options);
        WarningsList.ItemsSource = plan.Warnings;
        NoWarningsText.Visibility = plan.Warnings.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ChangesList.ItemsSource = plan.Operations.Select(operation => new ReviewChange(
            operation.Kind.ToString(),
            operation.Description,
            Path.GetRelativePath(plan.Game.GameDirectory, operation.TargetPath))).ToArray();
    }

    private static IReadOnlyList<ReviewSetting> BuildSettings(PatchOptions options)
    {
        var settings = new List<ReviewSetting>
        {
            State("Overlay proxy", options.InstallOverlayProxy),
            State("Steam overlay", options.LoadOverlay),
            State("Unlock DLC", options.UnlockAllDlc),
            State("Competing loaders", options.QuarantineCompetingFiles),
            State("Force ownership", options.ForceOwnership),
            State("SteamStub", options.EnableSteamStub),
            State("Ticket passthrough", options.PassthroughTicket),
            State("Ticket emulation", options.EmulateTicket),
            State("SDR", options.EnableSdr),
            State("Overlay logging", options.LogOverlay),
            State("Verbose logging", options.VerboseLog),
            State("Photon", options.InstallPhoton),
            State("EOS", options.InstallEos),
            State("PlayFab", options.InstallPlayFab),
            State("coherence", options.InstallCoherence)
        };
        if (!string.IsNullOrWhiteSpace(options.EosClientSecret))
            settings.Add(new ReviewSetting("EOS credentials", "CONFIGURED"));
        if (options.AdditionalSettings.Count > 0)
            settings.Add(new ReviewSetting("Additional flags", options.AdditionalSettings.Count.ToString()));
        return settings;
    }

    private static ReviewSetting State(string name, bool enabled) => new(name, enabled ? "ON" : "OFF");

    private void Confirm_Click(object sender, RoutedEventArgs e) => DialogResult = true;

    private sealed record ReviewSetting(string Name, string Value);
    private sealed record ReviewChange(string Action, string Description, string RelativePath);
}
