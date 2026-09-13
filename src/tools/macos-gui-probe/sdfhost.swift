// What SwiftUI and AppKit actually put in a CASDFLayer, measured on the host. Calculator's bezels
// cannot be read directly (AMFI blocks exec, SIP blocks attach), so this builds the same widgets --
// SwiftUI Buttons in a keypad grid, plus the stock AppKit controls -- and dumps every CASDF*/SDF*
// layer in the resulting tree with the full SDF property surface.
//
// Swift rather than Objective-C because a SwiftUI View value cannot be constructed from ObjC; the
// SDF classes only appear once SwiftUI has laid something out.
//
// Build: swiftc -O -o /tmp/sdfprobe/sdfhost sdfhost.swift
import AppKit
import SwiftUI
import QuartzCore
import ObjectiveC.runtime

let sdfLayerClass: AnyClass? = objc_getClass("CASDFLayer") as? AnyClass
let sdfElementClass: AnyClass? = objc_getClass("CASDFElementLayer") as? AnyClass

func describeColor(_ value: Any?) -> String {
    guard let value else { return "nil" }
    let color = value as! CGColor
    let comps = color.components ?? []
    return "[" + comps.map { String(format: "%.4f", $0) }.joined(separator: " ") + "] space=" + (color.colorSpace?.name as String? ?? "?")
}

func describeEffect(_ effect: Any?) -> String {
    guard let effect = effect as? NSObject else { return "nil" }
    let cls: AnyClass = type(of: effect)
    var out = String(cString: class_getName(cls))
    var count: UInt32 = 0
    if let props = class_copyPropertyList(cls, &count) {
        for i in 0..<Int(count) {
            let name = String(cString: property_getName(props[i]))
            let attrs = String(cString: property_getAttributes(props[i])!)
            let value = effect.value(forKey: name)
            if attrs.contains("T^{CGColor") {
                out += " \(name)=\(describeColor(value))"
            } else if let array = value as? [Any] {
                let items = array.map { item -> String in
                    if CFGetTypeID(item as CFTypeRef) == CGColor.typeID { return describeColor(item) }
                    return "\(item)"
                }
                out += " \(name)=[\(items.joined(separator: ", "))]"
            } else {
                out += " \(name)=\(value.map { "\($0)" } ?? "nil")"
            }
        }
        free(props)
    }
    return out
}

func dump(_ layer: CALayer, _ depth: Int, _ onlySdf: Bool) {
    let name = String(cString: object_getClassName(layer))
    let isSdf = name.contains("SDF") || name.contains("sdf")
    if !onlySdf || isSdf {
        let pad = String(repeating: "  ", count: depth)
        let b = layer.bounds
        var line = String(
            format: "%@%@ b=(%.2f,%.2f,%.2f,%.2f) p=(%.2f,%.2f) a=(%.2f,%.2f) cr=%.2f curve=%@ hidden=%d op=%.2f masks=%d",
            pad, name, b.origin.x, b.origin.y, b.width, b.height, layer.position.x, layer.position.y,
            layer.anchorPoint.x, layer.anchorPoint.y, layer.cornerRadius, layer.cornerCurve.rawValue,
            layer.isHidden ? 1 : 0, Double(layer.opacity), layer.masksToBounds ? 1 : 0)
        if let bg = layer.backgroundColor { line += " bg=\(describeColor(bg))" }
        if let f = layer.filters, !f.isEmpty { line += " filters=\(f.map { String(cString: object_getClassName($0 as AnyObject)) })" }
        if let f = layer.backgroundFilters, !f.isEmpty { line += " bgFilters=\(f.map { String(cString: object_getClassName($0 as AnyObject)) })" }
        if let f = layer.compositingFilter { line += " comp=\(String(cString: object_getClassName(f as AnyObject)))" }
        if let m = layer.mask { line += " mask=\(String(cString: object_getClassName(m)))" }
        if name.contains("Backdrop") {
            let b = layer as NSObject
            line += String(format: " | backdrop groupName=%@ scale=%@ backdropRect=%@ bleed=%@ zoom=%@ enabled=%@ captureOnly=%@ inPlace=%@ substitute=%@",
                           "\(b.value(forKey: "groupName") ?? "nil")", "\(b.value(forKey: "scale") ?? "?")",
                           "\(b.value(forKey: "backdropRect") ?? "?")", "\(b.value(forKey: "bleedAmount") ?? "?")",
                           "\(b.value(forKey: "zoom") ?? "?")", "\(b.value(forKey: "enabled") ?? "?")",
                           "\(b.value(forKey: "captureOnly") ?? "?")", "\(b.value(forKey: "allowsInPlaceFiltering") ?? "?")",
                           describeColor(b.value(forKey: "substituteColor")))
        }
        if name.contains("Portal") {
            let b = layer as NSObject
            let src = b.value(forKey: "sourceLayer") as? CALayer
            line += String(format: " | portal source=%@ srcBounds=%@ hides=%@ matchesPosition=%@ matchesTransform=%@ matchesOpacity=%@",
                           src.map { String(cString: object_getClassName($0)) } ?? "nil",
                           src.map { "\($0.bounds)" } ?? "-",
                           "\(b.value(forKey: "hidesSourceLayer") ?? "?")", "\(b.value(forKey: "matchesPosition") ?? "?")",
                           "\(b.value(forKey: "matchesTransform") ?? "?")", "\(b.value(forKey: "matchesOpacity") ?? "?")")
        }
        if layer.contents != nil { line += " contents=\(String(cString: object_getClassName(layer.contents as AnyObject)))" }
        if let cls = sdfElementClass, layer.isKind(of: cls) {
            line += String(format: " | mode=%@ operation=%@ z0=%g z1=%g ovalization=%g hitTestsAsFill=%@",
                           layer.value(forKey: "mode") as? String ?? "?",
                           layer.value(forKey: "operation") as? String ?? "?",
                           (layer.value(forKey: "contentsZeroValueDistance") as? Double) ?? .nan,
                           (layer.value(forKey: "contentsOneValueDistance") as? Double) ?? .nan,
                           (layer.value(forKey: "gradientOvalization") as? Double) ?? .nan,
                           "\((layer.value(forKey: "hitTestsAsFill") as? Bool) ?? false)")
        }
        if let cls = sdfLayerClass, layer.isKind(of: cls) {
            line += String(format: " | smoothness=%g gaussianRadius=%g effectOffset=%g mergeElements=%@ effect=%@",
                           (layer.value(forKey: "smoothness") as? Double) ?? .nan,
                           (layer.value(forKey: "gaussianRadius") as? Double) ?? .nan,
                           (layer.value(forKey: "effectOffset") as? Double) ?? .nan,
                           "\((layer.value(forKey: "mergeElements") as? Bool) ?? false)",
                           describeEffect(layer.value(forKey: "effect")))
        }
        print(line)
    }
    for child in layer.sublayers ?? [] {
        dump(child, depth + 1, onlySdf)
    }
}

struct Keypad: View {
    var body: some View {
        ZStack {
            LinearGradient(colors: [.black, .white], startPoint: .leading, endPoint: .trailing)
            VStack(spacing: 10) {
                GlassEffectContainer(spacing: 6) {
                    HStack(spacing: 6) {
                        ForEach(0..<3) { _ in
                            Color.clear.frame(width: 44, height: 44)
                                .glassEffect(.regular, in: .rect(cornerRadius: 12))
                        }
                    }
                }
                Color.clear.frame(width: 120, height: 40).glassEffect(.regular, in: .capsule)
                Color.clear.frame(width: 120, height: 40).glassEffect(.regular.tint(.orange), in: .capsule)
            }
        }
        .frame(width: 220, height: 200)
    }
}

final class Delegate: NSObject, NSApplicationDelegate {
    var swiftUIWindow: NSWindow!
    var appKitWindow: NSWindow!

    func applicationDidFinishLaunching(_ note: Notification) {
        if ProcessInfo.processInfo.environment["SDFHOST_DARK"] != nil {
            NSApp.appearance = NSAppearance(named: .darkAqua)
        }
        let hosting = NSHostingView(rootView: Keypad())
        hosting.frame = NSRect(x: 200, y: 200, width: 220, height: 200)
        swiftUIWindow = NSWindow(contentRect: hosting.frame,
                                 styleMask: [.borderless],
                                 backing: .buffered, defer: false)
        swiftUIWindow.title = "sdfhost-swiftui"
        swiftUIWindow.isOpaque = true
        swiftUIWindow.backgroundColor = .black
        swiftUIWindow.contentView = hosting
        swiftUIWindow.makeKeyAndOrderFront(nil)

        appKitWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 320, height: 240),
                                styleMask: [.titled, .closable],
                                backing: .buffered, defer: false)
        appKitWindow.title = "sdfhost-appkit"
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 240))
        container.wantsLayer = true
        let push = NSButton(title: "push", target: nil, action: nil)
        push.frame = NSRect(x: 20, y: 180, width: 100, height: 32)
        let recessed = NSButton(title: "recessed", target: nil, action: nil)
        recessed.bezelStyle = .recessed
        recessed.frame = NSRect(x: 150, y: 180, width: 120, height: 32)
        let segmented = NSSegmentedControl(labels: ["a", "b", "c"], trackingMode: .selectOne, target: nil, action: nil)
        segmented.frame = NSRect(x: 20, y: 130, width: 200, height: 26)
        let toggle = NSSwitch(frame: NSRect(x: 20, y: 90, width: 40, height: 24))
        let slider = NSSlider(frame: NSRect(x: 20, y: 50, width: 200, height: 24))
        var extra: [NSView] = [push, recessed, segmented, toggle, slider]
        if let glassClass = objc_getClass("NSGlassEffectView") as? NSView.Type {
            let glass = glassClass.init(frame: NSRect(x: 230, y: 40, width: 70, height: 70))
            extra.append(glass)
        }
        for v in extra { container.addSubview(v) }
        appKitWindow.contentView = container
        appKitWindow.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            print("### SwiftUI window: every layer")
            dump(self.swiftUIWindow.contentView!.layer!, 0, false)
            print("\n### SwiftUI window: SDF layers only")
            dump(self.swiftUIWindow.contentView!.layer!, 0, true)
            print("\n### AppKit window: SDF layers only")
            dump(self.appKitWindow.contentView!.layer!, 0, true)
            print("\n### AppKit window: every layer")
            dump(self.appKitWindow.contentView!.layer!, 0, false)
            self.capture()
            exit(0)
        }
    }
}

extension Delegate {
    func capture() {
        typealias Fn = @convention(c) (CGRect, UInt32, CGWindowID, UInt32) -> Unmanaged<CGImage>?
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else {
            print("CAPTURE-FAIL no symbol"); return
        }
        let fn = unsafeBitCast(sym, to: Fn.self)
        guard let image = fn(.null, 1 << 3, CGWindowID(swiftUIWindow.windowNumber), 1 << 0)?.takeRetainedValue() else {
            print("CAPTURE-FAIL nil image"); return
        }
        let w = image.width, h = image.height
        let rep = NSBitmapImageRep(cgImage: image)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "/tmp/sdfprobe/glass.png"))
        print("CAPTURE \(w)x\(h) -> /tmp/sdfprobe/glass.png")
    }
}

let app = NSApplication.shared
let delegate = Delegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
