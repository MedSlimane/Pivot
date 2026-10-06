import AppKit
import CoreGraphics

// Pivot — menu bar utility for monitor rotation + arrangement.
// Rotation: MonitorPanel private framework via MPShim (ObjC, linked from
// MPShim.m). Arrangement: public CG API (CGBegin/CGConfigureOrigin/Complete).
// Zero polling: state is refreshed by NSApplication.didChangeScreenParameters
// notification (kernel/WindowServer event) and user actions only.

@_silgen_name("MPShimCanChangeOrientation")
func MPShimCanChangeOrientation(_ displayID: UInt32) -> Int32
@_silgen_name("MPShimGetOrientation")
func MPShimGetOrientation(_ displayID: UInt32) -> Int32
@_silgen_name("MPShimSetOrientation")
func MPShimSetOrientation(_ displayID: UInt32, _ degrees: Int32) -> Int32

struct DisplayBox {
    let id: CGDirectDisplayID
    let name: String
    let pixelSize: CGSize
    let isMain: Bool
    let canRotate: Bool
    var origin: CGPoint      // screen-space origin (tentative while dragging)
    var size: CGSize         // screen-space size (swaps with rotation)
    var rotation: Int32      // 0/90/180/270

    var frame: CGRect { CGRect(origin: origin, size: size) }
}

enum PivotDisplays {
    static func snapshot() -> [DisplayBox] {
        var count: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &count)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard count > 0 else { return [] }
        CGGetActiveDisplayList(count, &ids, &count)

        var names: [CGDirectDisplayID: String] = [:]
        for screen in NSScreen.screens {
            if let n = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber {
                if #available(macOS 14.0, *) {
                    names[n.uint32Value] = screen.localizedName
                }
            }
        }

        var boxes: [DisplayBox] = []
        for id in ids {
            let b = CGDisplayBounds(id)
            let mode = CGDisplayCopyDisplayMode(id)
            let rot = CGDisplayRotation(id)
            let degrees = Int32(rot.rounded())
            // Pixel dims of the panel (mode dims are pre-rotation).
            var w = mode.map { CGFloat($0.width) } ?? b.width
            var h = mode.map { CGFloat($0.height) } ?? b.height
            if degrees == 90 || degrees == 270 { swap(&w, &h) }
            boxes.append(DisplayBox(
                id: id,
                name: names[id] ?? "Display \(id)",
                pixelSize: CGSize(width: w, height: h),
                isMain: CGDisplayIsMain(id) != 0,
                canRotate: MPShimCanChangeOrientation(id) == 1,
                origin: b.origin,
                size: b.size,
                rotation: degrees
            ))
        }
        return boxes
    }

    static func applyRotation(_ id: CGDirectDisplayID, _ degrees: Int32) {
        _ = MPShimSetOrientation(id, degrees)
        // Screen-params notification refreshes state; nudge once for snappy UI.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
            NotificationCenter.default.post(name: NSApplication.didChangeScreenParametersNotification, object: nil)
        }
    }

    static func applyOrigin(_ id: CGDirectDisplayID, _ origin: CGPoint) {
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        let err = CGConfigureDisplayOrigin(config, id, Int32(origin.x), Int32(origin.y))
        if err == .success {
            CGCompleteDisplayConfiguration(config, .permanently)
        } else {
            CGCancelDisplayConfiguration(config)
        }
    }

    static func openDisplaysSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Displays-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }
}

// MARK: - Menu bar controller

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var canvasController: ArrangeCanvasController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.behavior = .terminationOnRemoval
        statusItem.autosaveName = "Pivot"

        if let img = NSImage(systemSymbolName: "rectangle.landscape.rotate", accessibilityDescription: "Pivot") {
            img.isTemplate = true
            statusItem.button?.image = img
        } else if let img = NSImage(systemSymbolName: "arrow.triangle.2.circlepath", accessibilityDescription: "Pivot") {
            img.isTemplate = true
            statusItem.button?.image = img
        }
        statusItem.button?.toolTip = "Pivot — monitor rotation & arrangement"

        rebuildMenu()

        NotificationCenter.default.addObserver(
            self, selector: #selector(screensChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

    @objc private func screensChanged() {
        rebuildMenu()
        canvasController?.reload()
    }

    private func rebuildMenu() {
        let boxes = PivotDisplays.snapshot()
        let menu = NSMenu()
        menu.autoenablesItems = false

        let status = NSMenuItem(title: boxes.isEmpty
            ? "Status: no displays"
            : "Status: \(boxes.count) display\(boxes.count == 1 ? "" : "s")",
            action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)
        menu.addItem(.separator())

        for box in boxes {
            let parent = NSMenuItem(
                title: "\(box.name) — \(Int(box.size.width))×\(Int(box.size.height)), \(box.rotation)°",
                action: nil, keyEquivalent: "")
            let sub = NSMenu()
            for deg in [0, 90, 180, 270] {
                let item = NSMenuItem(
                    title: deg == 0 ? "0° (standard)" : "\(deg)°",
                    action: #selector(rotateDisplay(_:)), keyEquivalent: "")
                item.target = self
                item.tag = deg
                item.representedObject = Int(box.id)
                item.state = box.rotation == Int32(deg) ? .on : .off
                if !box.canRotate { item.isEnabled = false }
                sub.addItem(item)
            }
            sub.addItem(.separator())
            let place = NSMenuItem(
                title: "Place at \(Int(box.origin.x)), \(Int(box.origin.y))",
                action: nil, keyEquivalent: "")
            place.isEnabled = false
            sub.addItem(place)
            parent.submenu = sub
            menu.addItem(parent)
        }

        menu.addItem(.separator())
        let canvas = NSMenuItem(
            title: "Arrange Displays…", action: #selector(toggleCanvas), keyEquivalent: "a")
        canvas.target = self
        menu.addItem(canvas)
        let settings = NSMenuItem(
            title: "Displays Settings…", action: #selector(openSettings), keyEquivalent: "")
        settings.target = self
        menu.addItem(settings)
        menu.addItem(.separator())
        let quit = NSMenuItem(
            title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu
    }

    @objc private func rotateDisplay(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? Int else { return }
        PivotDisplays.applyRotation(CGDirectDisplayID(id), Int32(sender.tag))
    }

    @objc private func toggleCanvas() {
        if let cc = canvasController, cc.window.isVisible {
            cc.window.orderOut(nil)
        } else {
            canvasController = ArrangeCanvasController()
            canvasController?.show()
        }
    }

    @objc private func openSettings() { PivotDisplays.openDisplaysSettings() }
    @objc private func quit() { NSApp.terminate(nil) }
}

// MARK: - Arrange canvas

final class ArrangeCanvasController: NSObject, NSWindowDelegate {
    let window: NSWindow
    private let view = ArrangeView(frame: NSRect(x: 0, y: 0, width: 640, height: 440))

    override init() {
        window = NSWindow(
            contentRect: view.frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        super.init()
        window.title = "Pivot — Arrange Displays"
        window.minSize = NSSize(width: 480, height: 340)
        window.isReleasedWhenClosed = false
        window.contentView = view
        window.delegate = self
    }

    func show() {
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func reload() { view.reload() }

    func windowWillClose(_ notification: Notification) {
        (NSApp.delegate as? AppDelegate)?.canvasClosed()
    }
}

final class ArrangeView: NSView {
    private var boxes: [DisplayBox] = []
    private var draggedIndex: Int?
    private var hoverIndex: Int?
    private var dragOffset = CGSize()
    private let snapPx: CGFloat = 24
    private let pad: CGFloat = 28

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        reload()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func reload() {
        boxes = PivotDisplays.snapshot()
        needsDisplay = true
    }

    private var unionRect: CGRect {
        var u = CGRect.null
        for b in boxes { u = u.union(b.frame) }
        return u.isNull ? CGRect(x: 0, y: 0, width: 1, height: 1) : u
    }

    private var scale: CGFloat {
        let u = unionRect
        let sx = max(1, (bounds.width - 2 * pad)) / u.width
        let sy = max(1, (bounds.height - 2 * pad)) / u.height
        return min(sx, sy)
    }

    private func rectFor(_ b: DisplayBox) -> NSRect {
        let u = unionRect
        let s = scale
        let x = pad + (b.frame.minX - u.minX) * s
        let y = pad + (u.maxY - b.frame.maxY) * s
        return NSRect(x: x, y: y, width: b.frame.width * s, height: b.frame.height * s)
    }

    private func box(at point: NSPoint) -> Int? {
        for (i, b) in boxes.enumerated() where rectFor(b).contains(point) {
            return i
        }
        return nil
    }

    override func draw(_ rect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        bounds.fill()

        for (i, b) in boxes.enumerated() {
            let r = rectFor(b).insetBy(dx: 2, dy: 2)
            let path = NSBezierPath(roundedRect: r, xRadius: 10, yRadius: 10)

            let fill: NSColor = {
                if i == draggedIndex { return .controlAccentColor.withAlphaComponent(0.55) }
                if i == hoverIndex { return .controlAccentColor.withAlphaComponent(0.35) }
                return .controlAccentColor.withAlphaComponent(0.18)
            }()
            fill.setFill()
            path.fill()

            let stroke: NSColor = {
                if i == draggedIndex { return .controlAccentColor }
                if b.isMain { return .controlAccentColor.withAlphaComponent(0.9) }
                return .controlAccentColor.withAlphaComponent(0.45)
            }()
            stroke.setStroke()
            path.lineWidth = b.isMain ? 2.5 : 1.5
            path.stroke()

            if r.width > 90 && r.height > 52 {
                let name = NSAttributedString(string: b.name, attributes: [
                    .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
                    .foregroundColor: NSColor.labelColor,
                ])
                let meta = NSAttributedString(
                    string: "\(Int(b.pixelSize.width))×\(Int(b.pixelSize.height)) · \(b.rotation)°",
                    attributes: [
                        .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .regular),
                        .foregroundColor: NSColor.secondaryLabelColor,
                    ])
                let total = name.size().height + meta.size().height + 3
                name.draw(at: NSPoint(
                    x: r.midX - name.size().width / 2,
                    y: r.midY + total / 2 - name.size().height))
                meta.draw(at: NSPoint(
                    x: r.midX - meta.size().width / 2,
                    y: r.midY - total / 2))
            }
        }

        let hint = NSAttributedString(
            string: "Drag to arrange · double-click or right-click a display to rotate",
            attributes: [
                .font: NSFont.systemFont(ofSize: 10),
                .foregroundColor: NSColor.secondaryLabelColor,
            ])
        hint.draw(at: NSPoint(
            x: bounds.midX - hint.size().width / 2,
            y: 8))
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        guard let i = box(at: p) else { return }
        if event.clickCount == 2 {
            if boxes[i].canRotate {
                let next = (boxes[i].rotation + 90) % 360
                PivotDisplays.applyRotation(boxes[i].id, next)
            } else {
                NSSound.beep()
            }
            return
        }
        draggedIndex = i
        let r = rectFor(boxes[i])
        let world = screenPoint(for: p)
        dragOffset = CGSize(width: world.x - boxes[i].origin.x, height: world.y - boxes[i].origin.y)
        _ = r
        needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard let i = draggedIndex else { return }
        let p = convert(event.locationInWindow, from: nil)
        let world = screenPoint(for: p)
        var o = CGPoint(
            x: (world.x - dragOffset.width).rounded(),
            y: (world.y - dragOffset.height).rounded())
        o.x = min(max(o.x, -32000), 32000)
        o.y = min(max(o.y, -32000), 32000)
        boxes[i].origin = o
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        guard let i = draggedIndex else { return }
        draggedIndex = nil
        boxes[i].origin = snap(boxes[i].origin, excluding: boxes[i].id)
        let id = boxes[i].id
        let origin = boxes[i].origin
        PivotDisplays.applyOrigin(id, origin)
        needsDisplay = true
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let h = box(at: p)
        if h != hoverIndex {
            hoverIndex = h
            needsDisplay = true
        }
    }

    override func rightMouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        guard let i = box(at: p) else { return }
        let b = boxes[i]
        let menu = NSMenu()
        for deg in [0, 90, 180, 270] {
            let item = NSMenuItem(
                title: deg == 0 ? "0° (standard)" : "\(deg)°",
                action: #selector(rotateFromCanvas(_:)), keyEquivalent: "")
            item.target = self
            item.tag = deg
            item.representedObject = Int(b.id)
            item.state = b.rotation == Int32(deg) ? .on : .off
            if !b.canRotate { item.isEnabled = false }
            menu.addItem(item)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }

    @objc private func rotateFromCanvas(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? Int else { return }
        PivotDisplays.applyRotation(CGDirectDisplayID(id), Int32(sender.tag))
    }

    private func screenPoint(for viewPoint: NSPoint) -> CGPoint {
        let u = unionRect
        let s = scale
        return CGPoint(
            x: u.minX + (viewPoint.x - pad) / s,
            y: u.maxY - (viewPoint.y - pad) / s)
    }

    private func snap(_ origin: CGPoint, excluding id: CGDirectDisplayID) -> CGPoint {
        var o = origin
        let moving = CGRect(origin: origin, size: boxes.first(where: { $0.id == id })?.size ?? .zero)
        for other in boxes where other.id != id {
            let threshold = snapPx
            if abs(moving.maxX - other.frame.minX) < threshold {
                o.x = other.frame.minX - moving.width; return o
            }
            if abs(moving.minX - other.frame.maxX) < threshold {
                o.x = other.frame.maxX; return o
            }
            if abs(moving.maxY - other.frame.minY) < threshold {
                o.y = other.frame.minY - moving.height; return o
            }
            if abs(moving.minY - other.frame.maxY) < threshold {
                o.y = other.frame.maxY; return o
            }
        }
        return o
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(
            rect: bounds, options: [.mouseMoved, .activeAlways], owner: self))
    }
}

extension AppDelegate {
    func canvasClosed() {}
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
