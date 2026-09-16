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

    func float(_ section: String, _ key: String, default def: Double) -> Double {
        guard let raw = get(section, key), let v = Double(raw) else { return def }
        return v
    }

    func setFloat(_ section: String, _ key: String, _ value: Double) {
        set(section, key, String(format: "%.2f", value))
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
    func binding(_ section: String, _ key: String, default def: Double) -> Binding<Double> {
        Binding(get: { self.float(section, key, default: def) },
                set: { self.setFloat(section, key, $0) })
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

    // Launcher-only preference (not an .ini key -- this toggles Apple's own
    // built-in Metal HUD overlay via the MTL_HUD_ENABLED environment
    // variable at launch time, not anything this backend implements itself.
    // Works because MoltenVK is a real Metal client underneath Vulkan.
    @AppStorage("VCSShowMetalHUD") private var showMetalHUD: Bool = false

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
    private var hardwareTransform: Binding<Bool> { doc.binding("Rendering", "HardwareTransform", default: false) }
    private var anisotropic: Binding<Int> { doc.binding("Rendering", "AnisotropicFiltering", default: 1) }
    private var sharpen: Binding<Double> { doc.binding("Rendering", "Sharpen", default: 0.0) }
    private var depthPrecision: Binding<Int> { doc.binding("Rendering", "DepthPrecision", default: 24) }
    private var fxaaEnabled: Binding<Bool> { doc.binding("Rendering", "SMAA", default: false) }

    // Timing / widescreen / addons
    private var widescreen: Binding<Bool> { doc.binding("Widescreen", "Enabled", default: true) }
    private var project2dfx: Binding<Bool> { doc.binding("Project2DFX", "Enabled", default: false) }

    // Bloom (Vulkan backend only -- see the [SimulateHDR] comment in the ini)
    private var bloomEnabled: Binding<Bool> { doc.binding("SimulateHDR", "Enabled", default: false) }
    private var bloomThreshold: Binding<Double> { doc.binding("SimulateHDR", "Threshold", default: 0.80) }
    private var bloomIntensity: Binding<Double> { doc.binding("SimulateHDR", "Intensity", default: 0.60) }

    // Color grading ("Vice Neon" preset)
    private var gradingEnabled: Binding<Bool> { doc.binding("ColorGrading", "Enabled", default: false) }
    private var gradingSaturation: Binding<Double> { doc.binding("ColorGrading", "Saturation", default: 1.25) }
    private var gradingContrast: Binding<Double> { doc.binding("ColorGrading", "Contrast", default: 1.08) }
    private var gradingBrightness: Binding<Double> { doc.binding("ColorGrading", "Brightness", default: 0.0) }
    private var gradingTintR: Binding<Double> { doc.binding("ColorGrading", "TintR", default: 1.05) }
    private var gradingTintG: Binding<Double> { doc.binding("ColorGrading", "TintG", default: 0.98) }
    private var gradingTintB: Binding<Double> { doc.binding("ColorGrading", "TintB", default: 1.02) }

    // Draw distance
    private var drawDistanceEnabled: Binding<Bool> { doc.binding("DrawDistance", "Enabled", default: false) }
    private var drawDistanceWorld: Binding<Double> { doc.binding("DrawDistance", "World", default: 1.00) }
    private var drawDistanceVehicles: Binding<Double> { doc.binding("DrawDistance", "Vehicles", default: 1.00) }
    private var drawDistanceNPCs: Binding<Double> { doc.binding("DrawDistance", "NPCs", default: 1.00) }
    private var drawDistanceLOD: Binding<Double> { doc.binding("DrawDistance", "LOD", default: 1.00) }

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
                    Toggle("Show Metal HUD (FPS + GPU frame time)", isOn: $showMetalHUD)
                        .help("Apple's own built-in Metal performance overlay (top-left corner) — live FPS and GPU frame time, straight from the system, not this game's own counter. Works here because MoltenVK (this build's Vulkan-on-Metal layer) is a real Metal client. A launcher preference, not an .ini setting.")
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
                    LabeledContent("Texture sharpening") {
                        Slider(value: sharpen, in: 0.0...1.0, step: 0.05)
                            .frame(width: 160)
                        Text(String(format: "%.2f", sharpen.wrappedValue))
                            .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                    }
                    .help("Unsharp-mask sharpening of the final frame. 0 = off. PSP-era textures are low-resolution and get magnified a lot by the internal-resolution upscale — this makes existing detail read as crisper, it doesn't invent detail that was never captured. 0.3-0.6 is a reasonable range before halos/ringing become visible. Works independently of Color Grading.")
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

                Section("Bloom") {
                    Toggle("Bloom (experimental)", isOn: bloomEnabled)
                        .help("Adds a soft glow around bright highlights (streetlights, headlights, neon signs, sun glare). A genuinely new single-pass implementation, verified live — but only on the Vulkan GE backend: this Mac build, or a Windows build compiled with PSPRECOMP_VCS_WIN32_VULKAN=ON. A default Windows (DirectX12) build silently ignores this toggle.")
                    if bloomEnabled.wrappedValue {
                        LabeledContent("Threshold") {
                            Slider(value: bloomThreshold, in: 0.0...1.0, step: 0.05)
                                .frame(width: 160)
                            Text(String(format: "%.2f", bloomThreshold.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                        }
                        .help("Luminance (0-1) above which a pixel starts glowing. Lower = more of the scene blooms; higher = only the brightest highlights do.")
                        LabeledContent("Intensity") {
                            Slider(value: bloomIntensity, in: 0.0...2.0, step: 0.05)
                                .frame(width: 160)
                            Text(String(format: "%.2f", bloomIntensity.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                        }
                        .help("How strongly the glow is added back over the scene. Additive light — high values can blow out highlights to solid white.")
                    }
                }

                Section("Color grading") {
                    Toggle("\"Vice Neon\" look", isOn: gradingEnabled)
                        .help("A researched approximation of GTA:VC's classic Miami neon aesthetic (boosted saturation, a warm sunset-leaning tint) — tuned against real ENB/ReShade community presets, not a guess. Purely optional and off by default. Honest limitation: this is one static grade, not the original game's actual day/night color-cycle system, so it can't separately push warm daytime vs cool neon-night tones.")
                    if gradingEnabled.wrappedValue {
                        LabeledContent("Saturation") {
                            Slider(value: gradingSaturation, in: 0.5...2.0, step: 0.05)
                                .frame(width: 160)
                            Text(String(format: "%.2f", gradingSaturation.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                        }
                        .help("Color intensity. 1.0 = unchanged. The 1.15-1.30 range is the sweet spot before it starts looking oversaturated/cartoonish.")
                        LabeledContent("Contrast") {
                            Slider(value: gradingContrast, in: 0.5...1.5, step: 0.02)
                                .frame(width: 160)
                            Text(String(format: "%.2f", gradingContrast.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                        }
                        .help("Contrast around mid-gray. 1.0 = unchanged. Higher values start crushing shadow detail.")
                        LabeledContent("Brightness") {
                            Slider(value: gradingBrightness, in: -0.3...0.3, step: 0.02)
                                .frame(width: 160)
                            Text(String(format: "%.2f", gradingBrightness.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                        }
                        .help("Flat brightness offset. 0.0 = unchanged. Vice City's look relies on saturation/hue, not raised black levels — keep this near zero.")
                        LabeledContent("Warm tint (R/G/B)") {
                            Slider(value: gradingTintR, in: 0.8...1.3, step: 0.01)
                                .frame(width: 90)
                            Slider(value: gradingTintG, in: 0.8...1.3, step: 0.01)
                                .frame(width: 90)
                            Slider(value: gradingTintB, in: 0.8...1.3, step: 0.01)
                                .frame(width: 90)
                        }
                        .help("Multiplies red/green/blue independently. 1.0/1.0/1.0 = unchanged. The default (slightly up on red, slightly down on green, mildly up on blue) approximates sunset-neon warmth without a full day/night system.")
                    }
                }

                Section("Draw distance") {
                    Toggle("Extended draw distance", isOn: drawDistanceEnabled)
                        .help("Scales up how far away world objects, vehicles and pedestrians render/despawn, instead of the PSP's original short pop-in range. A real patch to the game's own LOD/culling code, verified live.")
                    if drawDistanceEnabled.wrappedValue {
                        LabeledContent("World objects") {
                            Slider(value: drawDistanceWorld, in: 1.0...8.0, step: 0.25)
                                .frame(width: 160)
                            Text(String(format: "%.2f×", drawDistanceWorld.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 44, alignment: .trailing)
                        }
                        .help("Multiplies world/scenery draw distance and the game's far clip plane. 1.0 = original PSP distance, up to 8.0×.")
                        LabeledContent("Vehicles") {
                            Slider(value: drawDistanceVehicles, in: 1.0...4.0, step: 0.25)
                                .frame(width: 160)
                            Text(String(format: "%.2f×", drawDistanceVehicles.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 44, alignment: .trailing)
                        }
                        .help("Multiplies vehicle spawn/despawn range, up to 4.0×. Values above 2.0× are untested for mission-trigger compatibility.")
                        LabeledContent("Pedestrians") {
                            Slider(value: drawDistanceNPCs, in: 1.0...4.0, step: 0.25)
                                .frame(width: 160)
                            Text(String(format: "%.2f×", drawDistanceNPCs.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 44, alignment: .trailing)
                        }
                        .help("Multiplies pedestrian spawn/population range, up to 4.0×. Values above 2.0× are untested for mission-trigger compatibility.")
                        Note(text: "Above 2.0× on Vehicles/Pedestrians is untested for mission compatibility — the underlying patch itself caps at 4.0×.", tint: .orange)
                        LabeledContent("Model detail switch") {
                            Slider(value: drawDistanceLOD, in: 1.0...10.0, step: 0.5)
                                .frame(width: 160)
                            Text(String(format: "%.2f×", drawDistanceLOD.wrappedValue))
                                .foregroundStyle(.secondary).frame(width: 44, alignment: .trailing)
                        }
                        .help("Distance at which a vehicle/pedestrian switches from its low-poly to high-poly model — purely visual, no population-density downside (unlike Vehicles/Pedestrians above), so this can go much higher safely. Fixes the \"low-poly blob snapping to a full model a few meters away\" pop-in. Up to 10.0×.")
                    }
                }

                Section("Anti-aliasing") {
                    Toggle("FXAA", isOn: fxaaEnabled)
                        .help("Fast approximate anti-aliasing — smooths jagged polygon edges with a single post-process pass. A genuinely new, working Vulkan implementation, verified live. Not the real SMAA algorithm (which needs precomputed lookup textures and three passes) — kept under the historical \"SMAA\" .ini key for compatibility, but this is FXAA.")
                }

                Section("Not available on this build") {
                    LabeledContent("MSAA") { Text("Off").foregroundStyle(.tertiary) }
                        .help("Multisample anti-aliasing — smooths jagged polygon edges. Confirmed hardcoded off in this build's Vulkan backend (DirectX12-only feature); editing the .ini value has no effect. FXAA above is this build's real anti-aliasing option instead.")
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
                    LabeledContent("ProperShaders / CloudWorks color grading & clouds") { Text("Off").foregroundStyle(.tertiary) }
                        .help("DirectX12-only post-processing (color grading, volumetric clouds). Confirmed absent from this build's Vulkan backend entirely — no Mac equivalent exists yet.")
                    Note(text: "Color grading and volumetric clouds are DirectX12-only — confirmed absent from the Vulkan backend entirely. Bloom now has its own Vulkan implementation; see Quality → Bloom.")
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
                    LabeledContent("Target frame rate") { Text("60 fps").foregroundStyle(.secondary) }
                        .help("Locked to 60. This build is tuned and verified against a 60fps baseline (batch coalescing, GPU timestamp profiling, memoryless depth, etc.) — an uncapped or 120fps+ mode isn't offered since none of that tuning has been validated at a different target.")
                    Note(text: "Fixed at 60fps — the baseline this port is tuned and verified against, not a placeholder default.")
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
        // Defensive hygiene: Metal shader validation and MoltenVK's own
        // per-frame activity/performance logging both have a real per-frame
        // CPU cost and are meant for debug builds, not play sessions. Not
        // needed for a Finder double-click (a fresh process inherits no
        // shell environment at all), but launching through here can
        // otherwise inherit whatever the parent shell/IDE happened to have
        // exported (e.g. a Terminal session with MTL_SHADER_VALIDATION=1
        // set globally for other Metal work) -- forcing them off here means
        // a Play from this launcher is never accidentally slower than a
        // real double-click launch would be.
        var environment: [String: String] = [
            "MTL_SHADER_VALIDATION": "0",
            "MTL_DEBUG_LAYER": "0",
            "MVK_CONFIG_PERFORMANCE_TRACKING": "0",
        ]
        if showMetalHUD {
            environment["MTL_HUD_ENABLED"] = "1"
        }
        configuration.environment = environment
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
