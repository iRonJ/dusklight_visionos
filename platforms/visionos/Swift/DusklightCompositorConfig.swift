#if os(visionOS)

import CompositorServices
import SwiftUI

struct DusklightCompositorConfig: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.depthFormat = .depth32Float
        configuration.colorFormat = .bgra8Unorm_srgb

        // Keep bring-up output spatially linear. Applying the drawable's
        // foveation map with the current explicit per-eye passes warps eye 1
        // as the user's gaze moves.
        configuration.isFoveationEnabled = false

        let supportedLayouts = capabilities.supportedLayouts(options: [])

        configuration.layout = supportedLayouts.contains(.layered) ? .layered : .dedicated
    }
}

#endif // os(visionOS)
