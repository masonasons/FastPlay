import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    /// False if the audio engine could not start; the first window says so.
    private(set) var engineStarted = false

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // Before the engine reopens what was playing, which may be a server's file
        LoopbackServer.shared.startForRestore()
        engineStarted = FPEngine.shared.start()
        return true
    }

    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        UISceneConfiguration(name: "Default", sessionRole: connectingSceneSession.role)
    }

    func applicationWillTerminate(_ application: UIApplication) {
        FPEngine.shared.saveState()
    }
}
