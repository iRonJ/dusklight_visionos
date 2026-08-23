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
    @State private var isLaunching = false

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 52))
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(.tint)

            VStack(spacing: 4) {
                Text("Dusklight")
                    .font(.largeTitle.bold())
                Text("The Legend of Zelda: Twilight Princess")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Button {
                isLaunching = true
                Task {
                    _ = await openImmersiveSpace(id: "DusklightImmersiveSpace")
                    dismissWindow()
                }
            } label: {
                HStack(spacing: 8) {
                    if isLaunching {
                        ProgressView()
                    } else {
                        Image(systemName: "play.fill")
                    }
                    Text(isLaunching ? "Entering VR..." : "Play in Immersive VR")
                        .font(.headline)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .disabled(isLaunching)
        }
        .padding(32)
        .frame(width: 440, height: 300)
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
