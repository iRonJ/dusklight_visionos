#if os(visionOS)

import SwiftUI
import CompositorServices
import UniformTypeIdentifiers
import OSLog
import Spatial

private let logger = Logger(subsystem: "dev.twilitrealm.dusk", category: "App")

private actor DiscImporter {
    static let shared = DiscImporter()

    func importDisc(from sourceURL: URL) throws -> String {
        let accessing = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if accessing {
                sourceURL.stopAccessingSecurityScopedResource()
            }
        }

        logger.info("[DusklightSwift] Selected file: \(sourceURL.path) (security-scoped: \(accessing))")
        let documentsURL = FileManager.default.urls(
            for: .documentDirectory, in: .userDomainMask).first!
        let localURL = documentsURL.appendingPathComponent(sourceURL.lastPathComponent)

        if sourceURL.path == localURL.path || FileManager.default.fileExists(atPath: localURL.path) {
            return localURL.path
        }

        try Task.checkCancellation()
        try FileManager.default.copyItem(at: sourceURL, to: localURL)
        logger.info("[DusklightSwift] Successfully copied disc to: \(localURL.path)")
        return localURL.path
    }
}

@_silgen_name("dusklight_visionos_start")
func dusklight_visionos_start(_ layerRenderer: LayerRenderer)

@_silgen_name("dusklight_start_game_with_iso")
func dusklight_start_game_with_iso(_ isoPath: UnsafePointer<CChar>?)

@_silgen_name("dusklight_start_game_thread")
func dusklight_start_game_thread()

@_silgen_name("dusklight_visionos_is_game_running")
func dusklight_visionos_is_game_running() -> Bool

@_silgen_name("dusklight_visionos_set_app_active")
func dusklight_visionos_set_app_active(_ active: Bool)

@_silgen_name("dusklight_visionos_set_game_paused")
func dusklight_visionos_set_game_paused(_ paused: Bool)

@_silgen_name("dusklight_visionos_is_game_paused")
func dusklight_visionos_is_game_paused() -> Bool

@_silgen_name("dusklight_visionos_is_compositor_running")
func dusklight_visionos_is_compositor_running() -> Bool

@_silgen_name("dusklight_visionos_set_diorama_placement")
func dusklight_visionos_set_diorama_placement(
    _ x: Float, _ y: Float, _ z: Float, _ width: Float, _ aspectRatio: Float)

@_silgen_name("dusklight_visionos_recenter_diorama")
func dusklight_visionos_recenter_diorama()

@_silgen_name("dusklight_visionos_set_scene_plane_distance")
func dusklight_visionos_set_scene_plane_distance(_ distanceMeters: Float)

@MainActor
final class DioramaInteractionModel: ObservableObject {
    private struct Contact {
        var startPoint: Point3D
        var point: Point3D
        var startTime: TimeInterval
    }

    private struct TwoHandStart {
        var ids: [SpatialEventCollection.Event.ID]
        var midpoint: Point3D
        var separation: Double
        var x: Double
        var y: Double
        var z: Double
        var width: Double
    }

    @Published var x = 0.0
    @Published var y = 0.0
    @Published var distance = 1.5
    @Published var scenePlaneDistance = 6.0
    @Published var width = 1.6
    @Published var aspectIndex = 0

    private var contacts: [SpatialEventCollection.Event.ID: Contact] = [:]
    private var twoHandStart: TwoHandStart?
    private var suppressTap = false
    var onQuickPinch: (() -> Void)?

    private var aspectRatio: Double {
        aspectIndex == 0 ? 16.0 / 9.0 : 64.0 / 27.0
    }

    init() {
        dusklight_visionos_set_scene_plane_distance(Float(scenePlaneDistance))
    }

    func handle(_ events: SpatialEventCollection) {
        for event in events {
            guard event.trackingAreaIdentifier.rawValue == 1 else {
                continue
            }
            switch event.phase {
            case .active:
                if var contact = contacts[event.id] {
                    contact.point = event.location3D
                    contacts[event.id] = contact
                } else {
                    contacts[event.id] = Contact(
                        startPoint: event.location3D,
                        point: event.location3D,
                        startTime: event.timestamp)
                }
            case .ended, .cancelled:
                if event.phase == .ended,
                   !suppressTap,
                   contacts.count == 1,
                   let contact = contacts[event.id],
                   event.timestamp - contact.startTime < 0.45,
                   distance(contact.startPoint, event.location3D) < 0.05 {
                    if let onQuickPinch {
                        onQuickPinch()
                    } else {
                        cycleAspectRatio()
                    }
                }
                contacts.removeValue(forKey: event.id)
            @unknown default:
                break
            }
        }

        if contacts.count >= 2 {
            updateTwoHandGesture()
        } else {
            twoHandStart = nil
            if contacts.isEmpty {
                suppressTap = false
            }
        }
    }

    func setAspectIndex(_ index: Int) {
        aspectIndex = index == 0 ? 0 : 1
        publishPlacement()
    }

    func setWidth(_ newWidth: Double) {
        width = min(max(newWidth, 0.6), 3.2)
        publishPlacement()
    }

    func setDistance(_ newDistance: Double) {
        distance = min(max(newDistance, 0.75), 5.0)
        publishPlacement()
    }

    func setScenePlaneDistance(_ newDistance: Double) {
        scenePlaneDistance = min(max(newDistance, 1.5), 20.5)
        dusklight_visionos_set_scene_plane_distance(Float(scenePlaneDistance))
    }

    func recenter() {
        x = 0.0
        y = 0.0
        dusklight_visionos_recenter_diorama()
        publishPlacement()
    }

    func resetGesture() {
        contacts.removeAll()
        twoHandStart = nil
        suppressTap = false
    }

    private func updateTwoHandGesture() {
        if twoHandStart == nil {
            let ids = Array(contacts.keys.prefix(2))
            guard ids.count == 2,
                  let first = contacts[ids[0]],
                  let second = contacts[ids[1]] else { return }
            twoHandStart = TwoHandStart(
                ids: ids,
                midpoint: midpoint(first.point, second.point),
                separation: max(distance(first.point, second.point), 0.02),
                x: x,
                y: y,
                z: distance,
                width: width)
            suppressTap = true
        }

        guard let start = twoHandStart,
              let first = contacts[start.ids[0]],
              let second = contacts[start.ids[1]] else {
            twoHandStart = nil
            return
        }

        let currentMidpoint = midpoint(first.point, second.point)
        let scale = distance(first.point, second.point) / start.separation
        x = start.x + Double(currentMidpoint.x - start.midpoint.x)
        y = start.y + Double(currentMidpoint.y - start.midpoint.y)
        distance = min(max(
            start.z - Double(currentMidpoint.z - start.midpoint.z), 0.75), 5.0)
        width = min(max(start.width * scale, 0.6), 3.2)
        publishPlacement()
    }

    private func cycleAspectRatio() {
        aspectIndex = aspectIndex == 0 ? 1 : 0
        publishPlacement()
        logger.info("[DusklightSwift] Diorama aspect changed to \(self.aspectIndex == 0 ? "16:9" : "21:9")")
    }

    private func publishPlacement() {
        dusklight_visionos_set_diorama_placement(
            Float(x), Float(y), Float(distance), Float(width), Float(aspectRatio))
    }

    private func midpoint(_ lhs: Point3D, _ rhs: Point3D) -> Point3D {
        Point3D(
            x: (lhs.x + rhs.x) * 0.5,
            y: (lhs.y + rhs.y) * 0.5,
            z: (lhs.z + rhs.z) * 0.5)
    }

    private func distance(_ lhs: Point3D, _ rhs: Point3D) -> Double {
        let dx = Double(lhs.x - rhs.x)
        let dy = Double(lhs.y - rhs.y)
        let dz = Double(lhs.z - rhs.z)
        return (dx * dx + dy * dy + dz * dz).squareRoot()
    }
}

@MainActor
final class VisionSessionModel: ObservableObject {
    @Published var gameStarted = dusklight_visionos_is_game_running()
    @Published var immersiveOpen = false
    @Published var isOpeningImmersive = false
    @Published var controlsVisible = false
    @Published private(set) var gamePaused = dusklight_visionos_is_game_paused()
    let interaction = DioramaInteractionModel()

    func setGamePaused(_ paused: Bool) {
        gamePaused = paused
        dusklight_visionos_set_game_paused(paused)
    }

    func synchronizeGamePause() {
        gamePaused = dusklight_visionos_is_game_paused()
    }
}

struct LauncherView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject var session: VisionSessionModel
    @AppStorage("dusklight_last_iso_path") private var selectedIsoPath: String = ""
    @State private var showFilePicker = false
    @State private var isLaunching = false
    @State private var statusMessage = ""
    @State private var importTask: Task<Void, Never>?
    @State private var launchTask: Task<Void, Never>?

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
        .onDisappear {
            importTask?.cancel()
            if !session.immersiveOpen && !session.gameStarted {
                launchTask?.cancel()
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
            let fileName = url.lastPathComponent
            statusMessage = "Importing \(fileName)..."
            importTask?.cancel()
            importTask = Task(priority: .userInitiated) {
                do {
                    let finalPath = try await DiscImporter.shared.importDisc(from: url)
                    try Task.checkCancellation()
                    selectedIsoPath = finalPath
                    let discName = (finalPath as NSString).lastPathComponent
                    statusMessage = "Ready: " + discName
                    logger.info("[DusklightSwift] Set selectedIsoPath to: \(finalPath)")
                } catch is CancellationError {
                    return
                } catch {
                    let description = error.localizedDescription
                    statusMessage = "Import failed: " + description
                    logger.error("[DusklightSwift] Failed to copy disc: \(description)")
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
        session.isOpeningImmersive = true
        logger.info("[DusklightSwift] launchGame() clicked with resolved disc path: \(resolvedPath)")

        launchTask?.cancel()
        launchTask = Task {
            defer {
                isLaunching = false
                session.isOpeningImmersive = false
            }
            logger.info("[DusklightSwift] Opening DusklightImmersiveSpace...")
            let result = await openImmersiveSpace(id: "DusklightImmersiveSpace")
            guard !Task.isCancelled, scenePhase == .active else {
                logger.info("[DusklightSwift] Launch cancelled while opening immersive space")
                return
            }
            logger.info("[DusklightSwift] openImmersiveSpace result: \(String(describing: result))")
            switch result {
            case .opened:
                logger.info("[DusklightSwift] Immersive space opened successfully. Starting game engine...")
                session.immersiveOpen = true
                session.gameStarted = true
                resolvedPath.withCString { cPath in
                    dusklight_start_game_with_iso(cPath)
                }
            case .error:
                statusMessage = "Error opening Immersive Space"
                logger.error("[DusklightSwift] Failed to open immersive space")
            case .userCancelled:
                logger.info("[DusklightSwift] User cancelled immersive space")
            @unknown default:
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

struct DioramaSessionView: View {
    @ObservedObject var session: VisionSessionModel
    let resume: () -> Void
    let interacted: () -> Void
    let editingChanged: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Label("Dusklight Diorama", systemImage: "rectangle.on.rectangle.angled")
                    .font(.headline)
                Spacer()
                Circle()
                    .fill(session.gamePaused ? Color.orange :
                          (session.immersiveOpen ? Color.green : Color.secondary))
                    .frame(width: 8, height: 8)
            }

            Picker("Aspect", selection: Binding(
                get: { session.interaction.aspectIndex },
                set: {
                    interacted()
                    session.interaction.setAspectIndex($0)
                })) {
                Text("16:9").tag(0)
                Text("21:9").tag(1)
            }
            .pickerStyle(.segmented)

            HStack(spacing: 12) {
                Image(systemName: "arrow.left.and.right")
                    .foregroundStyle(.secondary)
                Slider(value: Binding(
                    get: { session.interaction.width },
                    set: { session.interaction.setWidth($0) }), in: 0.6...3.2,
                    onEditingChanged: editingChanged)
            }

            HStack(spacing: 12) {
                Image(systemName: "arrow.up.left.and.arrow.down.right")
                    .foregroundStyle(.secondary)
                Slider(value: Binding(
                    get: { session.interaction.distance },
                    set: { session.interaction.setDistance($0) }), in: 0.75...5.0,
                    onEditingChanged: editingChanged)
                Text(session.interaction.distance, format: .number.precision(.fractionLength(1)))
                    .monospacedDigit()
                    .frame(width: 34, alignment: .trailing)
                Text("m")
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 12) {
                Image(systemName: "square.3.layers.3d")
                    .foregroundStyle(.secondary)
                Slider(value: Binding(
                    get: { session.interaction.scenePlaneDistance },
                    set: { session.interaction.setScenePlaneDistance($0) }), in: 1.5...20.5,
                    onEditingChanged: editingChanged)
                Text(session.interaction.scenePlaneDistance,
                     format: .number.precision(.fractionLength(1)))
                    .monospacedDigit()
                    .frame(width: 34, alignment: .trailing)
                Text("m")
                    .foregroundStyle(.secondary)
            }

            HStack {
                Button(action: {
                    interacted()
                    session.interaction.recenter()
                }) {
                    Label("Recenter", systemImage: "viewfinder")
                }
                Spacer()
                if session.gamePaused {
                    Button {
                        interacted()
                        session.setGamePaused(false)
                        if !session.immersiveOpen {
                            resume()
                        }
                    } label: {
                        Label("Resume", systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                } else if session.immersiveOpen {
                    Button {
                        interacted()
                        session.setGamePaused(true)
                    } label: {
                        Label("Pause", systemImage: "pause.fill")
                    }
                } else {
                    Button(action: {
                        interacted()
                        resume()
                    }) {
                        Label("Resume", systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                }
            }
        }
        .padding(24)
        .frame(minWidth: 340, idealWidth: 400, maxWidth: 560,
               minHeight: 270, idealHeight: 300, maxHeight: 440)
        .simultaneousGesture(TapGesture().onEnded { interacted() })
    }
}

struct DusklightRootView: View {
    @ObservedObject var session: VisionSessionModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.scenePhase) private var scenePhase
    @State private var controlsAutoHideTask: Task<Void, Never>?

    var body: some View {
        Group {
            if session.gameStarted {
                DioramaSessionView(session: session) {
                    Task { await resumeImmersiveSpace() }
                } interacted: {
                    scheduleControlsAutoHide()
                } editingChanged: { editing in
                    if editing {
                        cancelControlsAutoHide()
                    } else {
                        scheduleControlsAutoHide()
                    }
                }
            } else {
                LauncherView(session: session)
            }
        }
        .onAppear {
            session.controlsVisible = true
            session.gameStarted = dusklight_visionos_is_game_running()
            session.synchronizeGamePause()
            if session.gameStarted && !session.immersiveOpen {
                Task { await resumeImmersiveSpace() }
            }
            scheduleControlsAutoHide()
        }
        .onDisappear {
            cancelControlsAutoHide()
            session.controlsVisible = false
        }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase != .active {
                cancelControlsAutoHide()
            } else if session.gameStarted && !session.immersiveOpen {
                Task { await resumeImmersiveSpace() }
            }
        }
        .onChange(of: session.gameStarted) { _, _ in
            scheduleControlsAutoHide()
        }
        .onChange(of: session.gamePaused) { _, _ in
            scheduleControlsAutoHide()
        }
        .onChange(of: session.immersiveOpen) { _, _ in
            scheduleControlsAutoHide()
        }
    }

    private func scheduleControlsAutoHide() {
        cancelControlsAutoHide()
        guard session.gameStarted, session.immersiveOpen, !session.gamePaused else { return }

        controlsAutoHideTask = Task {
            do {
                try await Task.sleep(nanoseconds: 8_000_000_000)
            } catch {
                return
            }
            guard !Task.isCancelled, scenePhase == .active,
                  session.immersiveOpen, !session.gamePaused else { return }
            dismissWindow(id: "main")
        }
    }

    private func cancelControlsAutoHide() {
        controlsAutoHideTask?.cancel()
        controlsAutoHideTask = nil
    }

    private func resumeImmersiveSpace() async {
        guard session.gameStarted,
              !session.immersiveOpen,
              !session.isOpeningImmersive,
              scenePhase == .active else { return }
        session.isOpeningImmersive = true
        defer { session.isOpeningImmersive = false }
        // A layer paused by Home View or a system message normally resumes in
        // place. Give it time to wake before asking visionOS for a replacement.
        for _ in 0..<20 {
            guard !Task.isCancelled, scenePhase == .active else { return }
            if dusklight_visionos_is_compositor_running() {
                session.immersiveOpen = true
                logger.info("[DusklightSwift] Existing immersive compositor resumed")
                return
            }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        guard !Task.isCancelled, scenePhase == .active else { return }
        let result = await openImmersiveSpace(id: "DusklightImmersiveSpace")
        guard !Task.isCancelled, scenePhase == .active else { return }
        if case .opened = result {
            session.immersiveOpen = true
            logger.info("[DusklightSwift] Resumed existing game in immersive space")
        } else {
            logger.info("[DusklightSwift] Immersive resume result: \(String(describing: result))")
        }
    }
}

struct DusklightImmersiveContent: CompositorContent {
    @ObservedObject var session: VisionSessionModel
    @Environment(\.openWindow) private var openWindow
    @Environment(\.scenePhase) private var scenePhase

    var body: some CompositorContent {
        let openControls = openWindow
        CompositorLayer(configuration: DusklightCompositorConfig()) { layerRenderer in
            session.interaction.onQuickPinch = { [weak session] in
                guard let session else { return }
                if session.controlsVisible {
                    session.interaction.setAspectIndex(
                        session.interaction.aspectIndex == 0 ? 1 : 0)
                } else {
                    openControls(id: "main")
                }
            }
            layerRenderer.onSpatialEvent = { events in
                session.interaction.handle(events)
            }
            session.immersiveOpen = true
            dusklight_visionos_set_app_active(scenePhase == .active)
            dusklight_visionos_start(layerRenderer)
        }
        .onAppear {
            session.immersiveOpen = true
            dusklight_visionos_set_app_active(scenePhase == .active)
        }
        .onDisappear {
            session.immersiveOpen = false
            session.interaction.resetGesture()
            dusklight_visionos_set_app_active(false)
        }
        .onChange(of: scenePhase) { _, newPhase in
            let active = newPhase == .active
            dusklight_visionos_set_app_active(active)
            if active {
                session.immersiveOpen = true
            } else {
                session.immersiveOpen = false
                session.interaction.resetGesture()
            }
        }
    }
}

@main
struct DusklightVisionApp: App {
    @StateObject private var session = VisionSessionModel()
    @State private var immersionStyle: ImmersionStyle = .mixed

    var body: some Scene {
        WindowGroup(id: "main") {
            DusklightRootView(session: session)
        }
        .defaultSize(width: 420, height: 240)
        .windowResizability(.contentMinSize)

        ImmersiveSpace(id: "DusklightImmersiveSpace") {
            DusklightImmersiveContent(session: session)
        }
        .immersionStyle(selection: $immersionStyle, in: .mixed, .full)
    }
}

#endif // os(visionOS)
