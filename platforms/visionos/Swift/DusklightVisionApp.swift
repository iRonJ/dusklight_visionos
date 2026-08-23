#if os(visionOS)

import SwiftUI
import CompositorServices

@_silgen_name("dusklight_visionos_start")
func dusklight_visionos_start(_ layerRenderer: LayerRenderer)

@_silgen_name("dusklight_start_game_thread")
func dusklight_start_game_thread()

@main
struct DusklightVisionApp: App {
    @State private var immersionStyle: ImmersionStyle = .full

    init() {
        dusklight_start_game_thread()
    }

    var body: some Scene {
        ImmersiveSpace {
            CompositorLayer(configuration: DusklightCompositorConfig()) { layerRenderer in
                dusklight_visionos_start(layerRenderer)
            }
        }
        .immersionStyle(selection: $immersionStyle, in: .full)
    }
}

#endif // os(visionOS)
