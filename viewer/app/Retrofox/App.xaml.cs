using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Retrofox
{
    public sealed partial class App : Application
    {
        public App()
        {
            this.InitializeComponent();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            var frame = Window.Current.Content as Frame;
            if (frame == null)
            {
                frame = new Frame();
                Window.Current.Content = frame;
            }
            if (frame.Content == null)
            {
                frame.Navigate(typeof(MainPage), e.Arguments);
            }
            else if (!string.IsNullOrEmpty(e.Arguments))
            {
                // Already running: a pinned page's tile opens its page
                var page = frame.Content as MainPage;
                if (page != null)
                    page.OpenFromTile(e.Arguments);
            }
            Window.Current.Activate();
        }
    }
}
