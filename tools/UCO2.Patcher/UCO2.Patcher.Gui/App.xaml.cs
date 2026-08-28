using System.Windows;
using UCO2.Patcher.Core;

namespace UCO2.Patcher.Gui;

public partial class App : Application
{
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Length >= 6 && e.Args[0].Equals("--apply-update", StringComparison.OrdinalIgnoreCase))
        {
            try
            {
                await SelfUpdateService.ApplyAsync(e.Args[1], e.Args[2], e.Args[3], int.Parse(e.Args[4]), e.Args[5]);
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "UCOnline2 update failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            Shutdown();
            return;
        }

        string? initialGame = null;
        bool refreshFix = false;
        for (int index = 0; index + 1 < e.Args.Length; index++)
        {
            if (e.Args[index].Equals("--game", StringComparison.OrdinalIgnoreCase))
                initialGame = e.Args[index + 1];
        }
        refreshFix = e.Args.Any(arg => arg.Equals("--refresh-fix", StringComparison.OrdinalIgnoreCase));
        new MainWindow(initialGame, refreshFix).Show();
    }
}
