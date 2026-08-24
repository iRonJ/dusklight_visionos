#if os(visionOS)

import CompositorServices
import Metal
import SwiftUI

struct DusklightCompositorConfig: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.depthFormat = .depth32Float
        configuration.colorFormat = .bgra8Unorm_srgb

        // The compositor's foveation map produces an asymmetric curl in the
        // explicit right-eye pass. Keep projection spatially linear and gain
        // resolution by supersampling the game-owned eye targets instead.
        configuration.isFoveationEnabled = false

        if capabilities.supportedTrackingAreasFormats.contains(.r16Uint) {
            configuration.trackingAreasFormat = .r16Uint
            configuration.trackingAreasUsage = [.renderTarget, .shaderRead]
        }

        let supportedLayouts = capabilities.supportedLayouts(options: [])

        configuration.layout = supportedLayouts.contains(.layered) ? .layered : .dedicated
    }
}

#endif // os(visionOS)
