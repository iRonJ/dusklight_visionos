#if os(visionOS)

import SwiftUI
import CompositorServices
import UniformTypeIdentifiers
import OSLog

private let logger = Logger(subsystem: "dev.twilitrealm.dusk", category: "App")

@_silgen_name("dusklight_visionos_start")
func dusklight_visionos_start(_ layerRenderer: LayerRenderer)

@_silgen_name("dusklight_start_game_with_iso")
func dusklight_start_game_with_iso(_ isoPath: UnsafePointer<CChar>?)

@_silgen_name("dusklight_start_game_thread")
func dusklight_start_game_thread()

struct LauncherView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @AppStorage("dusklight_last_iso_path") private var selectedIsoPath: String = ""
    @State private var showFilePicker = false
    @State private var isLaunching = false
    @State private var statusMessage = ""

    private let discExtensions = ["iso", "gcm", "rvz", "ciso", "wbfs"]

    private var selectedFileName: String {
        if selectedIsoPath.isEmpty {
            return "No disc image selected"
        }
        return URL(fileURLWithPath: selectedIsoPath).lastPathComponent
    }

    var body: some View {
        VStack(spacing: 24) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 48))
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(.tint)

            VStack(spacing: 6) {
                Text("Dusklight")
                    .font(.largeTitle.bold())
                Text("The Legend of Zelda: Twilight Princess")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            VStack(spacing: 8) {
                HStack {
                    Image(systemName: "opticaldisc")
                        .foregroundStyle(.secondary)
                    Text(selectedFileName)
                        .font(.callout)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .frame(maxWidth: .infinity)
                .background(.thinMaterial)
                .cornerRadius(10)

                Button {
                    showFilePicker = true
                } label: {
                    Label("Choose Disc Image (ISO / GCM / RVZ)...", systemImage: "folder")
                        .font(.subheadline)
                }
            }

            if !statusMessage.isEmpty {
                Text(statusMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Button {
                launchGame()
            } label: {
                HStack(spacing: 8) {
                    if isLaunching {
                        ProgressView()
                    } else {
                        Image(systemName: "play.fill")
                    }
                    Text(isLaunching ? "Entering Diorama VR..." : "Play in 3D Diorama")
                        .font(.headline)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .disabled(isLaunching)
        }
        .padding(32)
        .frame(width: 480, height: 380)
        .onAppear {
            if let resolvedPath = resolveDiscPath() {
                selectedIsoPath = resolvedPath
                statusMessage = "Ready: " + (resolvedPath as NSString).lastPathComponent
            } else if !selectedIsoPath.isEmpty {
                statusMessage = "Disc image not found. Choose it again."
                selectedIsoPath = ""
            }
        }
        .fileImporter(
            isPresented: $showFilePicker,
            allowedContentTypes: [
                .data,
                .diskImage,
                UTType(filenameExtension: "iso") ?? .data,
                UTType(filenameExtension: "gcm") ?? .data,
                UTType(filenameExtension: "ciso") ?? .data,
                UTType(filenameExtension: "rvz") ?? .data,
                UTType(filenameExtension: "wbfs") ?? .data
            ],
            allowsMultipleSelection: false,
            onCompletion: onFileImported
        )
    }

    private func onFileImported(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            let accessing = url.startAccessingSecurityScopedResource()
            logger.info("[DusklightSwift] Selected file: \(url.path) (security-scoped: \(accessing))")

            let fileName = url.lastPathComponent
            statusMessage = "Importing \(fileName)..."

            Task.detached(priority: .userInitiated) {
                defer {
                    if accessing {
                        url.stopAccessingSecurityScopedResource()
                    }
                }

                let docsDir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
                let localURL = docsDir.appendingPathComponent(fileName)

                if url.path != localURL.path && !FileManager.default.fileExists(atPath: localURL.path) {
                    do {
                        try FileManager.default.copyItem(at: url, to: localURL)
                        logger.info("[DusklightSwift] Successfully copied disc to: \(localURL.path)")
                    } catch {
                        logger.error("[DusklightSwift] Failed to copy disc: \(error.localizedDescription)")
                    }
                }

                let finalPath = FileManager.default.fileExists(atPath: localURL.path) ? localURL.path : url.path
                await MainActor.run {
                    selectedIsoPath = finalPath
                    let discName = (finalPath as NSString).lastPathComponent
                    statusMessage = "Ready: " + discName
                    logger.info("[DusklightSwift] Set selectedIsoPath to: \(finalPath)")
                }
            }
        case .failure(let error):
            let errDesc = error.localizedDescription
            logger.error("[DusklightSwift] File picker error: \(errDesc)")
            statusMessage = "Selection error: " + errDesc
        }
    }

    private func launchGame() {
        guard let resolvedPath = resolveDiscPath() else {
            statusMessage = "Disc image not found. Choose a disc image first."
            selectedIsoPath = ""
            return
        }

        selectedIsoPath = resolvedPath
        isLaunching = true
        logger.info("[DusklightSwift] launchGame() clicked with resolved disc path: \(resolvedPath)")

        Task {
            logger.info("[DusklightSwift] Opening DusklightImmersiveSpace...")
            let result = await openImmersiveSpace(id: "DusklightImmersiveSpace")
            logger.info("[DusklightSwift] openImmersiveSpace result: \(String(describing: result))")
            switch result {
            case .opened:
                logger.info("[DusklightSwift] Immersive space opened successfully. Starting game engine...")
                resolvedPath.withCString { cPath in
                    dusklight_start_game_with_iso(cPath)
                }
                dismissWindow()
            case .error:
                statusMessage = "Error opening Immersive Space"
                isLaunching = false
                logger.error("[DusklightSwift] Failed to open immersive space")
            case .userCancelled:
                isLaunching = false
                logger.info("[DusklightSwift] User cancelled immersive space")
            @unknown default:
                isLaunching = false
                break
            }
        }
    }

    private func resolveDiscPath() -> String? {
        let fileManager = FileManager.default
        let docsDir = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first!

        if !selectedIsoPath.isEmpty {
            let fileName = (selectedIsoPath as NSString).lastPathComponent
            let currentContainerURL = docsDir.appendingPathComponent(fileName)
            if fileManager.fileExists(atPath: currentContainerURL.path) {
                return currentContainerURL.path
            }
            if fileManager.fileExists(atPath: selectedIsoPath) {
                return selectedIsoPath
            }
        }

        guard let contents = try? fileManager.contentsOfDirectory(
            at: docsDir,
            includingPropertiesForKeys: nil
        ) else {
            return nil
        }
        return contents.first { discExtensions.contains($0.pathExtension.lowercased()) }?.path
    }
}

@main
struct DusklightVisionApp: App {
    @State private var immersionStyle: ImmersionStyle = .mixed

    var body: some Scene {
        WindowGroup(id: "main") {
            LauncherView()
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "DusklightImmersiveSpace") {
            CompositorLayer(configuration: DusklightCompositorConfig()) { layerRenderer in
                dusklight_visionos_start(layerRenderer)
            }
        }
        .immersionStyle(selection: $immersionStyle, in: .mixed, .full)
    }
}

#endif // os(visionOS)
