// Reads the Swift-side state of SwiftUI.SDFLayer -- `sdfStyle`, `sdfEffects`, `sdfSubsets` -- which is
// where the colour has to be: a Liquid Glass tint is never handed to CoreAnimation as a CGColor
// (measured: an orange-tinted capsule's CASDFEffect ivars hold only white), so SwiftUI must resolve it
// somewhere the ObjC runtime cannot see. Mirror() enumerates a Swift class's stored properties
// including private ones, which gives the shape and the names before any pointer arithmetic.
//
// Build: swiftc -O -o /tmp/sdfprobe/sdfstyle sdfstyle.swift
import AppKit
import SwiftUI
import QuartzCore
import ObjectiveC.runtime

func isColourish(_ text: String) -> Bool {
    let lowered = text.lowercased()
    return lowered.contains("color") || lowered.contains("colour") || lowered.contains("tint") ||
        lowered.contains("orange") || lowered.contains("rgba") || lowered.contains("srgb")
}

func reflect(_ value: Any, _ label: String, _ depth: Int, _ limit: Int) {
    let pad = String(repeating: "  ", count: depth)
    let mirror = Mirror(reflecting: value)
    let type = String(describing: mirror.subjectType)
    var summary = String(describing: value)
    if summary.count > 400 { summary = String(summary.prefix(400)) + "…" }

    let mark = isColourish(type) || isColourish(summary) ? "   <== COLOURISH" : ""
    print("\(pad)\(label): \(type) = \(summary)\(mark)")

    if depth >= limit { return }
    for child in mirror.children {
        reflect(child.value, child.label ?? "?", depth + 1, limit)
    }
}

func walk(_ layer: CALayer, _ found: inout [CALayer]) {
    let name = String(cString: object_getClassName(layer))
    if name.contains("SDFLayer") && !name.contains("CASDF") { found.append(layer) }
    for child in layer.sublayers ?? [] { walk(child, &found) }
}

struct Keypad: View {
    var body: some View {
        ZStack {
            Color.black
            VStack(spacing: 10) {
                Color.clear.frame(width: 120, height: 40).glassEffect(.regular, in: .capsule)
                Color.clear.frame(width: 120, height: 40).glassEffect(.regular.tint(.orange), in: .capsule)
            }
        }
        .frame(width: 220, height: 200)
    }
}

final class Delegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    func applicationDidFinishLaunching(_ note: Notification) {
        if ProcessInfo.processInfo.environment["SDFSTYLE_DARK"] != nil {
            NSApp.appearance = NSAppearance(named: .darkAqua)
        }
        let hosting = NSHostingView(rootView: Keypad())
        hosting.frame = NSRect(x: 0, y: 0, width: 220, height: 200)
        window = NSWindow(contentRect: hosting.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isOpaque = true
        window.backgroundColor = .black
        window.contentView = hosting
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            var layers: [CALayer] = []
            walk(self.window.contentView!.layer!, &layers)
            print("SwiftUI SDFLayer instances: \(layers.count)")
            for (index, layer) in layers.enumerated() {
                print("\n=== SDFLayer[\(index)] \(String(cString: object_getClassName(layer))) bounds=\(layer.bounds) ===")
                let mirror = Mirror(reflecting: layer)
                print("  stored properties: \(mirror.children.count)")
                for child in mirror.children {
                    reflect(child.value, child.label ?? "?", 1, 9)
                }
            }
            exit(0)
        }
    }
}

let app = NSApplication.shared
let delegate = Delegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
