#if os(visionOS)

import SwiftUI
import CompositorServices

@_silgen_name("dusklight_visionos_start")
func dusklight_visionos_start(_ layerRenderer: LayerRenderer)

@_silgen_name("dusklight_start_game_thread")
func dusklight_start_game_thread()

struct LauncherView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        VStack(spacing: 16) {
            ProgressView()
            Text("Starting Dusklight...")
                .font(.headline)
        }
        .padding(24)
        .task {
            _ = await openImmersiveSpace(id: "DusklightImmersiveSpace")
            dismissWindow()
        }
    }
}

@main
struct DusklightVisionApp: App {
    @State private var immersionStyle: ImmersionStyle = .full

    init() {
        dusklight_start_game_thread()
    }

    var body: some Scene {
        WindowGroup {
            LauncherView()
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "DusklightImmersiveSpace") {
            CompositorLayer(configuration: DusklightCompositorConfig()) { layerRenderer in
                dusklight_visionos_start(layerRenderer)
            }
        }
        .immersionStyle(selection: $immersionStyle, in: .full)
    }
}

#endif // os(visionOS)
