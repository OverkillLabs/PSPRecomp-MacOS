import SwiftUI
import AppKit

// MARK: - INI document model
//
// Edits VCSNative.ini in place, line by line, rather than fully
// reserializing it -- the shipped file's comments are the only
// documentation some of these settings have (valid ranges, what each
// ResolutionMode value means), so a naive "parse into a dictionary and
// rewrite" approach would silently delete all of that on first save.

final class IniDocument: ObservableObject {
    @Published var lines: [String] = []
    @Published var loadError: String?
    let fileURL: URL

    init(fileURL: URL) {
        self.fileURL = fileURL
        reload()
    }

    func reload() {
        guard let text = try? String(contentsOf: fileURL, encoding: .utf8) else {
            loadError = "Could not read \(fileURL.lastPathComponent) at \(fileURL.path)."
            lines = []
            return
        }
        loadError = nil
        lines = text.components(separatedBy: "\n")
    }

    func save() throws {
        let text = lines.joined(separator: "\n")
        try text.write(to: fileURL, atomically: true, encoding: .utf8)
    }

    /// Reads `Key=Value` under `[Section]`, trimmed. Case-sensitive on the
    /// section/key spelling the shipped .ini actually uses.
    func get(_ section: String, _ key: String) -> String? {
        var inSection = false
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("[") && trimmed.hasSuffix("]") {
                inSection = trimmed.dropFirst().dropLast() == section
                continue
            }
            guard inSection, !trimmed.hasPrefix(";"), let eq = trimmed.firstIndex(of: "=") else { continue }
            let k = trimmed[trimmed.startIndex..<eq].trimmingCharacters(in: .whitespaces)
            if k == key {
                return trimmed[trimmed.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }

    /// Rewrites the first `Key=...` line found under `[Section]` in place,
    /// keeping everything else (comments, blank lines, ordering) untouched.
    /// Appends a new line at the end of the section if the key is missing
    /// entirely (defensive -- every key this launcher exposes ships in the
    /// default .ini, so this path is not expected to run in practice).
    func set(_ section: String, _ key: String, _ value: String) {
        var inSection = false
        var sectionEndIndex: Int?
        for (index, line) in lines.enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("[") && trimmed.hasSuffix("]") {
                if inSection { sectionEndIndex = index; break }
                inSection = trimmed.dropFirst().dropLast() == section
                continue
            }
            guard inSection else { continue }
            if !trimmed.hasPrefix(";"), let eq = trimmed.firstIndex(of: "=") {
                let k = trimmed[trimmed.startIndex..<eq].trimmingCharacters(in: .whitespaces)
                if k == key {
                    lines[index] = "\(key)=\(value)"
                    return
                }
            }
            sectionEndIndex = index + 1
        }
        if let insertAt = sectionEndIndex {
            lines.insert("\(key)=\(value)", at: insertAt)
        }
    }

    func bool(_ section: String, _ key: String, default def: Bool = false) -> Bool {
        guard let raw = get(section, key)?.lowercased() else { return def }
        return raw == "1" || raw == "true" || raw == "yes" || raw == "on"
    }

    func setBool(_ section: String, _ key: String, _ value: Bool) {
        set(section, key, value ? "true" : "false")
    }

    func int(_ section: String, _ key: String, default def: Int) -> Int {
        guard let raw = get(section, key), let v = Int(raw) else { return def }
        return v
    }

    func setInt(_ section: String, _ key: String, _ value: Int) {
        set(section, key, String(value))
    }
}

// MARK: - Bindings between the form and the document

extension IniDocument {
    func binding(_ section: String, _ key: String, default def: Bool) -> Binding<Bool> {
        Binding(get: { self.bool(section, key, default: def) },
                set: { self.setBool(section, key, $0) })
    }
    func binding(_ section: String, _ key: String, default def: Int) -> Binding<Int> {
        Binding(get: { self.int(section, key, default: def) },
                set: { self.setInt(section, key, $0) })
    }
    func binding(_ section: String, _ key: String, default def: String) -> Binding<String> {
        Binding(get: { self.get(section, key) ?? def },
                set: { self.set(section, key, $0) })
    }
}

// MARK: - App locations
//
// The launcher lives beside VCSNative.app and VCSNative.ini (the same
// bin/<config> directory CMake already drops all three into), so both are
// found relative to the launcher's own bundle rather than a hardcoded path.

enum GameLocation {
    static var directory: URL {
        Bundle.main.bundleURL.deletingLastPathComponent()
    }
    static var appURL: URL { directory.appendingPathComponent("VCSNative.app") }
    // VCSNative reads its config relative to its OWN executable's directory
    // (see main.cpp's native_executable_directory()/initialize_vcs_
    // configuration() -- argv[0]-derived, not the launcher's location), which
    // for an app bundle is Contents/MacOS inside VCSNative.app, not a sibling
    // of it. This used to point at "<dir>/VCSNative.ini" next to the .app --
    // a file the game never reads at all. Every Save/Play from this launcher
    // was silently writing to that orphaned sibling file while the running
    // game kept reading its own bundled copy, untouched -- confirmed live:
    // toggling Fullscreen, ResolutionMode, ShowFPS through the launcher had
    // no effect on the actual game no matter what was picked, while editing
    // the bundled ini directly worked correctly every time.
    static var iniURL: URL {
        appURL.appendingPathComponent("Contents/MacOS/VCSNative.ini")
    }
}

// MARK: - Small shared row/note styles

/// A settings row inside a `Form`/`Section` using the native macOS 13+
/// LabeledContent look -- no hand-rolled card backgrounds or padding, just
/// the system's own grouped-list styling, which is what makes System
/// Settings.app read as "sleek" instead of a stack of boxes.
struct Note: View {
    let text: String
    var tint: Color = .secondary
    var body: some View {
        Text(text)
            .font(.caption)
            .foregroundStyle(tint)
            .fixedSize(horizontal: false, vertical: true)
    }
}

// MARK: - Sidebar categories

enum Category: String, CaseIterable, Identifiable {
    case window = "Window"
    case rendering = "Rendering"
    case quality = "Quality"
    case addons = "Addons"
    case performance = "Performance"

    var id: String { rawValue }
    var icon: String {
        switch self {
        case .window: return "macwindow"
        case .rendering: return "square.resize"
        case .quality: return "sparkles"
        case .addons: return "wand.and.stars"
        case .performance: return "gauge.with.dots.needle.67percent"
        }
    }
}

// MARK: - Main view

struct ContentView: View {
    @ObservedObject var doc: IniDocument
    @State private var selection: Category? = .window
    @State private var saveMessage: String?
    @State private var launching = false

    // Display
    private var fullscreen: Binding<Bool> { doc.binding("Display", "Fullscreen", default: true) }
    private var resolutionMode: Binding<String> { doc.binding("Display", "ResolutionMode", default: "Desktop") }
    private var width: Binding<Int> { doc.binding("Display", "Width", default: 1920) }
    private var height: Binding<Int> { doc.binding("Display", "Height", default: 1080) }
    private var aspectRatio: Binding<String> { doc.binding("Display", "AspectRatio", default: "Stretch") }
    private var upscaleFilter: Binding<String> { doc.binding("Display", "UpscaleFilter", default: "Bilinear") }
    private var integerScale: Binding<Bool> { doc.binding("Display", "IntegerScale", default: false) }
    private var showFPS: Binding<Bool> { doc.binding("Display", "ShowFPS", default: false) }

    // Rendering (internal resolution + quality)
    private var internalMode: Binding<String> { doc.binding("Rendering", "InternalResolutionMode", default: "Desktop") }
    private var internalScale: Binding<Int> { doc.binding("Rendering", "InternalScale", default: 2) }
    private var internalWidth: Binding<Int> { doc.binding("Rendering", "InternalWidth", default: 1920) }
    private var internalHeight: Binding<Int> { doc.binding("Rendering", "InternalHeight", default: 1080) }
    private var hardwareTransform: Binding<Bool> { doc.binding("Rendering", "HardwareTransform", default: true) }
    private var anisotropic: Binding<Int> { doc.binding("Rendering", "AnisotropicFiltering", default: 1) }
    private var depthPrecision: Binding<Int> { doc.binding("Rendering", "DepthPrecision", default: 24) }

    // Timing / widescreen / addons
    private var frameRate: Binding<Int> { doc.binding("Timing", "FrameRate", default: 120) }
    private var widescreen: Binding<Bool> { doc.binding("Widescreen", "Enabled", default: true) }
    private var project2dfx: Binding<Bool> { doc.binding("Project2DFX", "Enabled", default: false) }

    var body: some View {
        Group {
            if let error = doc.loadError {
                errorView(error)
            } else {
                NavigationSplitView {
                    sidebar
                } detail: {
                    VStack(spacing: 0) {
                        ScrollView {
                            detail
                                .padding(.horizontal, 20)
                                .padding(.top, 20)
                                .padding(.bottom, 28)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        Divider()
                        bottomBar
                    }
                    .navigationSplitViewColumnWidth(min: 420, ideal: 460)
                }
                .navigationSplitViewStyle(.balanced)
            }
        }
        .frame(minWidth: 720, idealWidth: 760, minHeight: 520, idealHeight: 600)
    }

    // MARK: Sidebar

    private var sidebar: some View {
        List(selection: $selection) {
            Section {
                ForEach(Category.allCases) { category in
                    Label(category.rawValue, systemImage: category.icon).tag(category)
                }
            } header: {
                VStack(alignment: .leading, spacing: 2) {
                    Text("VCS Launcher")
                        .font(.system(.title3, design: .rounded, weight: .semibold))
                        .foregroundStyle(.primary)
                        .textCase(nil)
                    Text("Vice City Stories")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textCase(nil)
                }
                .padding(.bottom, 8)
            }
        }
        .listStyle(.sidebar)
        .navigationSplitViewColumnWidth(min: 190, ideal: 200, max: 240)
    }

    // MARK: Detail (per category)

    @ViewBuilder private var detail: some View {
        switch selection ?? .window {
        case .window: windowSection
        case .rendering: renderingSection
        case .quality: qualitySection
        case .addons: addonsSection
        case .performance: performanceSection
        }
    }

    private func header(_ title: String, _ subtitle: String? = nil) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.system(.title2, design: .rounded, weight: .bold))
            if let subtitle {
                Text(subtitle).font(.callout).foregroundStyle(.secondary)
            }
        }
        .padding(.bottom, 4)
    }

    private var windowSection: some View {
        VStack(alignment: .leading, spacing: 18) {
            header("Window", "How VCSNative shows up on your display.")

            Form {
                Section {
                    Toggle("Fullscreen (borderless)", isOn: fullscreen)
                        .help("On: fills the entire display in a borderless window (alt-tab friendly, no real fullscreen switch). Off: runs in a regular titled window at the Resolution size below.")
                    Note(text: fullscreen.wrappedValue
                         ? "Fills the display in a borderless window."
                         : "Runs in a regular window at the size below.")
                }

                Section("Output") {
                    Picker("Resolution", selection: resolutionMode) {
                        Text("Match desktop").tag("Desktop")
                        Text("PSP native (480×272)").tag("PSP")
                        Text("Custom").tag("Custom")
                    }
                    .help("The size of the actual window/output surface. \"Match desktop\" fills your screen's native resolution; \"PSP native\" reproduces the original tiny 480×272 output; \"Custom\" lets you set an exact Width/Height below. This is separate from Internal Rendering resolution, which controls what the game actually renders at before it's scaled to this size.")
                    if resolutionMode.wrappedValue == "Custom" {
                        LabeledContent("Width") {
                            TextField("", value: width, formatter: NumberFormatter())
                                .frame(width: 90).multilineTextAlignment(.trailing)
                        }
                        .help("Output window width in pixels, used only when Resolution is set to Custom.")
                        LabeledContent("Height") {
                            TextField("", value: height, formatter: NumberFormatter())
                                .frame(width: 90).multilineTextAlignment(.trailing)
                        }
                        .help("Output window height in pixels, used only when Resolution is set to Custom.")
                    }
                    Picker("Aspect ratio", selection: aspectRatio) {
                        Text("Stretch").tag("Stretch")
                        Text("Preserve (letterbox)").tag("Preserve")
                    }
                    .help("Stretch: fills the whole window, distorting the image if the window's aspect ratio doesn't match the game's. Preserve: keeps the game's original proportions and adds black bars (letterbox/pillarbox) instead of distorting it.")
                    Picker("Upscale filter", selection: upscaleFilter) {
                        Text("Bilinear").tag("Bilinear")
                        Text("Nearest (pixelated)").tag("Nearest")
                    }
                    .help("How the rendered image is scaled up to fill the output size. Bilinear smooths the scaling (softer image). Nearest keeps hard pixel edges (a sharp, blocky/retro look) — most noticeable when Internal Rendering resolution is set well below your output size.")
                    Toggle("Integer scale", isOn: integerScale)
                        .help("Restricts upscaling to whole-number multiples (1×, 2×, 3×...) instead of any arbitrary size. Keeps pixels perfectly square/crisp — mainly relevant if you're running at or near PSP native resolution — at the cost of not always filling the window exactly.")
                }

                Section {
                    Toggle("Show FPS counter", isOn: showFPS)
                        .help("Shows the game's own on-screen frames-per-second counter (separate from the Metal HUD overlay macOS itself can show).")
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        }
    }

    private var renderingSection: some View {
        VStack(alignment: .leading, spacing: 18) {
            header("Internal Rendering", "What the game actually renders at, independent of window size.")

            Form {
                Section("Resolution") {
                    Picker("Internal resolution", selection: internalMode) {
                        Text("Match desktop").tag("Desktop")
                        Text("PSP native (480×272)").tag("PSP")
                        Text("Scale × PSP native").tag("Scale")
                        Text("Custom").tag("Custom")
                    }
                    .help("What the GE (graphics engine) actually renders the game at, before the Window section's Output resolution scales it to fill your screen. Higher internal resolution means sharper geometry and text, at a real GPU cost. \"Scale\" multiplies the PSP's native 480×272 by a whole number below.")
                    if internalMode.wrappedValue == "Scale" {
                        Stepper("Scale: \(internalScale.wrappedValue)×", value: internalScale, in: 1...8)
                            .help("Multiplies PSP-native 480×272 by this factor (1-8×) to get the internal render resolution — e.g. 4× = 1920×1088.")
                    }
                    if internalMode.wrappedValue == "Custom" {
                        LabeledContent("Width") {
                            TextField("", value: internalWidth, formatter: NumberFormatter())
                                .frame(width: 90).multilineTextAlignment(.trailing)
                        }
                        .help("Internal render width in pixels, used only when Internal resolution is set to Custom.")
                        LabeledContent("Height") {
                            TextField("", value: internalHeight, formatter: NumberFormatter())
                                .frame(width: 90).multilineTextAlignment(.trailing)
                        }
                        .help("Internal render height in pixels, used only when Internal resolution is set to Custom.")
                    }
                }

                Section("Filtering") {
                    Picker("Anisotropic filtering", selection: anisotropic) {
                        Text("Off").tag(1)
                        Text("2×").tag(2)
                        Text("4×").tag(4)
                        Text("8×").tag(8)
                        Text("16×").tag(16)
                    }
                    .help("Sharpens textures viewed at a steep angle (roads, sidewalks stretching into the distance) instead of letting them blur. Higher values look better but cost a little GPU time; the PSP itself had no such thing, so this is a pure upgrade over the original.")
                    Picker("Depth precision", selection: depthPrecision) {
                        Text("16-bit").tag(16)
                        Text("24-bit").tag(24)
                        Text("32-bit").tag(32)
                    }
                    .help("Precision of the depth (Z) buffer used to decide which surface is in front of another. Higher precision reduces \"z-fighting\" flicker between overlapping surfaces, especially over VCS's long draw distances. 24-bit isn't natively available on Apple's Metal, so this automatically falls back to the closest format the GPU actually supports (usually 32-bit).")
                    Note(text: "Both genuinely affect rendering on this Vulkan/MoltenVK build. Depth precision falls back automatically to the closest format Metal actually supports if the exact bit depth isn't available.")
                }

                Section {
                    Toggle("GPU hardware transform", isOn: hardwareTransform)
                        .help("Moves vertex transform/lighting math from the CPU onto the GPU. Faster in principle, but this Mac build's implementation has a confirmed rendering bug (distorted, textureless geometry) — leave off unless you're specifically testing it.")
                    Note(text: "Experimental — known rendering corruption on this Vulkan build. Leave off unless you're specifically testing it.", tint: .orange)
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        }
    }

    private var qualitySection: some View {
        VStack(alignment: .leading, spacing: 18) {
            header("Quality")

            Form {
                Section {
                    Toggle("Widescreen (16:9)", isOn: widescreen)
                        .help("Widens the PSP's original 4:3-ish projection to 16:9 (matching a modern screen) instead of showing the original narrower field of view stretched or pillarboxed.")
                }

                Section("Not available on this build") {
                    LabeledContent("MSAA") { Text("Off").foregroundStyle(.tertiary) }
                        .help("Multisample anti-aliasing — smooths jagged polygon edges. Confirmed hardcoded off in this build's Vulkan backend (DirectX12-only feature); editing the .ini value has no effect.")
                    LabeledContent("SMAA") { Text("Off").foregroundStyle(.tertiary) }
                        .help("Subpixel Morphological Anti-Aliasing — a post-process edge-smoothing pass. Confirmed entirely absent from this build's Vulkan backend (DirectX12-only feature); editing the .ini value has no effect.")
                    Note(text: "MSAA and SMAA are DirectX12-only — confirmed absent from the Vulkan/MoltenVK backend entirely (MSAA is hardcoded to 1 sample, no SMAA pass exists). Not shown as editable controls since changing them in the .ini would have no effect.")
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        }
    }

    private var addonsSection: some View {
        VStack(alignment: .leading, spacing: 18) {
            header("Addons")

            Form {
                Section {
                    Toggle("Project2DFX", isOn: project2dfx)
                        .help("Night-time visual upgrade: glowing light coronas on streetlights/windows/signs, blinking and traffic-light glows, a procedural star field, and a raised helicopter ceiling. Purely cosmetic — genuinely cross-platform, confirmed working on this Mac build.")
                    Note(text: "Light coronas and LOD lights. Genuinely cross-platform — confirmed working on this Mac build. Off by default.")
                }

                Section("Not available on this build") {
                    LabeledContent("ProperShaders / CloudWorks") { Text("Off").foregroundStyle(.tertiary) }
                        .help("DirectX12-only post-processing chain (bloom, color grading, volumetric clouds). Confirmed absent from this build's Vulkan backend entirely — no Mac equivalent exists yet.")
                    Note(text: "DirectX12-only post-processing (bloom, color grading, volumetric clouds) — confirmed absent from the Vulkan backend entirely. No Mac equivalent exists yet.")
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        }
    }

    private var performanceSection: some View {
        VStack(alignment: .leading, spacing: 18) {
            header("Performance")

            Form {
                Section {
                    Picker("Target frame rate", selection: frameRate) {
                        Text("60").tag(60)
                        Text("120").tag(120)
                        Text("Uncapped (240)").tag(240)
                    }
                    .help("Caps how fast the game loop and renderer are allowed to run. Higher isn't necessarily smoother — it just raises the ceiling; actual performance still depends on what your Mac can sustain each frame.")
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        }
    }

    // MARK: Bottom bar

    private var bottomBar: some View {
        HStack(spacing: 12) {
            if let saveMessage {
                Text(saveMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .transition(.opacity)
            }
            Spacer()
            Button("Save") { save() }
                .keyboardShortcut("s", modifiers: .command)
                .buttonStyle(.bordered)
            Button {
                save()
                launch()
            } label: {
                Label(launching ? "Launching…" : "Play", systemImage: "play.fill")
                    .frame(minWidth: 76)
            }
            .keyboardShortcut(.defaultAction)
            .buttonStyle(.borderedProminent)
            .disabled(launching)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
        .background(.regularMaterial)
    }

    private func errorView(_ message: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 40))
                .foregroundStyle(.orange)
            Text(message)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
            Button("Retry") { doc.reload() }
                .buttonStyle(.borderedProminent)
        }
        .padding(48)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: Actions

    private func save() {
        do {
            try doc.save()
            withAnimation { saveMessage = "Saved" }
        } catch {
            withAnimation { saveMessage = "Could not save: \(error.localizedDescription)" }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            withAnimation { saveMessage = nil }
        }
    }

    private func launch() {
        launching = true
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: GameLocation.appURL, configuration: configuration) { _, error in
            DispatchQueue.main.async {
                launching = false
                if let error {
                    saveMessage = "Could not launch VCSNative: \(error.localizedDescription)"
                } else {
                    NSApp.terminate(nil)
                }
            }
        }
    }
}

@main
struct VCSLauncherApp: App {
    @StateObject private var doc = IniDocument(fileURL: GameLocation.iniURL)
    var body: some Scene {
        WindowGroup {
            ContentView(doc: doc)
        }
        .windowResizability(.contentSize)
        .windowToolbarStyle(.unifiedCompact)
    }
}
