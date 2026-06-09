import SwiftUI

@main
struct CompanionRobotApp: App {
    @StateObject private var viewModel = RobotViewModel()

    var body: some Scene {
        WindowGroup {
            NavigationStack {
                DeviceListView()
            }
            .environmentObject(viewModel)
            .onAppear {
                viewModel.startUp()
            }
        }
    }
}
