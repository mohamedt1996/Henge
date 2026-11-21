import SwiftUI
import UIKit

/// A Mac app's screen (Mac apps with windows, experimental): the windows Henge's AppKit hands over, scaled to fit,
/// with the Mac's pointer drawn over them, fingers as a trackpad (or, in touch mode, as a mouse where they land), and
/// the iOS keyboard as a Mac keyboard.
final class MacScreenUIView: UIView, UIKeyInput, UIGestureRecognizerDelegate {
    private let stage = CALayer()
    private var holders: [CALayer] = []
    var screen = CGSize(width: 1180, height: 680)

    /// The Mac's pointer, drawn over the screen (fingers have none): in the Mac's points, origin bottom left.
    private var cursor = CGPoint(x: 590, y: 340)
    private let arrow = CAShapeLayer()
    private var touchGestures: [UIGestureRecognizer] = []
    private var padGestures: [UIGestureRecognizer] = []

    /// Trackpad mode (the default): fingers work the pointer as on a Mac's trackpad. Off: touch where to click.
    static let trackpadKey = "henge.macapps.trackpad"
    var trackpad = UserDefaults.standard.object(forKey: MacScreenUIView.trackpadKey) as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(trackpad, forKey: Self.trackpadKey)
            applyMode()
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.08, alpha: 1)
        isMultipleTouchEnabled = true // two-finger taps and slides (a view gets one touch otherwise)
        stage.anchorPoint = .zero
        stage.isGeometryFlipped = true // the Mac's way up: origin at the bottom left
        stage.masksToBounds = true
        stage.backgroundColor = UIColor(white: 0.16, alpha: 1).cgColor
        layer.addSublayer(stage)
        // Touch mode: a tap clicks where it lands; one finger scrolls, as everywhere on iOS; two drag with the mouse
        // button down (select, move); a long press right-clicks.
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(doubleTapped(_:)))
        doubleTap.numberOfTapsRequired = 2
        tap.require(toFail: doubleTap)
        let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        scroll.maximumNumberOfTouches = 1
        let pan = UIPanGestureRecognizer(target: self, action: #selector(panned(_:)))
        pan.minimumNumberOfTouches = 2
        let press = UILongPressGestureRecognizer(target: self, action: #selector(pressed(_:)))
        touchGestures = [tap, doubleTap, pan, scroll, press]
        // Trackpad mode: one finger moves the pointer, a tap clicks at it (two quick taps double-click), two fingers
        // tap to right-click and slide to scroll, and a press held a moment then moved drags.
        let padMove = UIPanGestureRecognizer(target: self, action: #selector(padMoved(_:)))
        padMove.maximumNumberOfTouches = 1
        let padTap = UITapGestureRecognizer(target: self, action: #selector(padTapped(_:)))
        let padRight = UITapGestureRecognizer(target: self, action: #selector(padRightTapped(_:)))
        padRight.numberOfTouchesRequired = 2
        let padScroll = UIPanGestureRecognizer(target: self, action: #selector(padScrolled(_:)))
        padScroll.minimumNumberOfTouches = 2
        padScroll.maximumNumberOfTouches = 2
        let padHold = UILongPressGestureRecognizer(target: self, action: #selector(padHeld(_:)))
        padHold.minimumPressDuration = 0.3
        padHold.allowableMovement = 10
        padMove.delegate = self
        padHold.delegate = self
        padGestures = [padMove, padTap, padRight, padScroll, padHold]
        for g in touchGestures + padGestures {
            g.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
            addGestureRecognizer(g)
        }
        // A mouse or trackpad attached to the device: its pointer is the Mac's pointer, its button held down drags,
        // its other button right-clicks, its scrolling scrolls.
        let wheel = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        wheel.allowedScrollTypesMask = .all
        wheel.allowedTouchTypes = []
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        let drag = UIPanGestureRecognizer(target: self, action: #selector(dragged(_:)))
        drag.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        drag.maximumNumberOfTouches = 1
        let click = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        click.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        // Only a mouse's or trackpad's: a finger's tap has no buttons, and this took it as a right click where the
        // finger was (the pointer jumped there and nothing was clicked).
        let rightClick = UITapGestureRecognizer(target: self, action: #selector(rightClicked(_:)))
        rightClick.buttonMaskRequired = .secondary
        rightClick.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        for g in [wheel, hover, drag, click, rightClick] as [UIGestureRecognizer] { addGestureRecognizer(g) }
        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(pinched(_:)))
        pinch.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        addGestureRecognizer(pinch)
        // The pointer: the Mac's arrow, white with a dark edge, its tip at the point.
        let path = UIBezierPath()
        path.move(to: .zero)
        for p in [CGPoint(x: 0, y: 17), CGPoint(x: 4.2, y: 12.8), CGPoint(x: 7.2, y: 19.4), CGPoint(x: 10, y: 18.2),
                  CGPoint(x: 7.1, y: 11.8), CGPoint(x: 12.6, y: 11.8)] { path.addLine(to: p) }
        path.close()
        arrow.path = path.cgPath
        arrow.fillColor = UIColor.white.cgColor
        arrow.strokeColor = UIColor.black.cgColor
        arrow.lineWidth = 1.1
        arrow.lineJoin = .round
        arrow.anchorPoint = .zero
        arrow.bounds = CGRect(x: 0, y: 0, width: 13, height: 20)
        arrow.zPosition = 10
        arrow.shadowOpacity = 0.35
        arrow.shadowRadius = 1.5
        arrow.shadowOffset = CGSize(width: 0, height: 1)
        layer.addSublayer(arrow)
        applyMode()
        inputAssistantItem.leadingBarButtonGroups = [] // no shortcut bar over the Mac's screen
        inputAssistantItem.trailingBarButtonGroups = []
    }

    private func applyMode() {
        touchGestures.forEach { $0.isEnabled = !trackpad }
        padGestures.forEach { $0.isEnabled = trackpad }
        placeArrow()
    }

    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        padGestures.contains(g) && padGestures.contains(other)
    }

    /// The arrow at the pointer (the stage is flipped: Core Animation converts).
    private func placeArrow() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        arrow.position = layer.convert(cursor, from: stage)
        CATransaction.commit()
    }

    private func moveCursor(to p: CGPoint) {
        cursor = CGPoint(x: min(max(p.x, 0), screen.width - 1), y: min(max(p.y, 0), screen.height - 1))
        arrow.isHidden = false
        placeArrow()
        if trackpad { followPointer() }
    }

    private func pointerEvent(_ type: Int32, clicks: Int32 = 1, flags: UInt = 0) {
        henge_tl_app_mouse(type, cursor.x, cursor.y, clicks, flags)
    }

    // MARK: trackpad mode

    private var dragging = false
    private var lastHold = CGPoint.zero
    private var lastTap = Date.distantPast
    private var lastTapAt = CGPoint.zero

    /// Finger movement in this view to the pointer's: as far on the screen as the finger went, a little more.
    private func pointerDelta(_ d: CGPoint) -> CGPoint {
        let s = 1.6 / max(shown, 0.01)
        return CGPoint(x: d.x * s, y: -d.y * s) // the Mac's y runs up
    }

    @objc private func padMoved(_ g: UIPanGestureRecognizer) {
        guard !dragging, g.state == .changed else { return }
        let t = g.translation(in: self)
        g.setTranslation(.zero, in: self)
        let d = pointerDelta(t)
        moveCursor(to: CGPoint(x: cursor.x + d.x, y: cursor.y + d.y))
        pointerEvent(5, clicks: 0)
    }

    @objc private func padTapped(_ g: UITapGestureRecognizer) {
        let quick = Date().timeIntervalSince(lastTap) < 0.4 && hypot(cursor.x - lastTapAt.x, cursor.y - lastTapAt.y) < 6
        let clicks: Int32 = quick ? 2 : 1
        lastTap = Date()
        lastTapAt = cursor
        pointerEvent(1, clicks: clicks)
        pointerEvent(2, clicks: clicks)
    }

    @objc private func padRightTapped(_ g: UITapGestureRecognizer) {
        pointerEvent(3)
        pointerEvent(4)
    }

    private var lastPadScroll = CGPoint.zero
    @objc private func padScrolled(_ g: UIPanGestureRecognizer) {
        let t = g.translation(in: self)
        let phase: Int32 = g.state == .began ? 1 : (g.state == .ended || g.state == .cancelled) ? 8 : 4
        if g.state == .began { lastPadScroll = .zero }
        let s = max(shown, 0.01)
        henge_tl_app_scroll(cursor.x, cursor.y, (t.x - lastPadScroll.x) / s, (t.y - lastPadScroll.y) / s, phase)
        lastPadScroll = t
    }

    @objc private func padHeld(_ g: UILongPressGestureRecognizer) {
        let p = g.location(in: self)
        switch g.state {
        case .began:
            dragging = true
            lastHold = p
            pointerEvent(1)
        case .changed:
            let d = pointerDelta(CGPoint(x: p.x - lastHold.x, y: p.y - lastHold.y))
            lastHold = p
            moveCursor(to: CGPoint(x: cursor.x + d.x, y: cursor.y + d.y))
            pointerEvent(6)
        case .ended, .cancelled, .failed:
            if dragging { pointerEvent(2) }
            dragging = false
        default: break
        }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        // The screen takes the keyboard from the start (a hardware keyboard types into the app at once); the on-screen
        // one comes only with the keyboard button.
        if window != nil { DispatchQueue.main.async { _ = self.becomeFirstResponder() } }
    }

    required init?(coder: NSCoder) { fatalError() }

    private var fit: CGFloat { min(bounds.width / max(screen.width, 1), bounds.height / max(screen.height, 1)) }

    /// Pinch zoom (1x: the whole screen fits) and where the zoomed screen is moved to (from centred, view points).
    private var zoom: CGFloat = 1
    private var offset = CGPoint.zero
    private var shown: CGFloat { fit * zoom }

    override func layoutSubviews() {
        super.layoutSubviews()
        placeStage()
    }

    /// The screen at the zoom, centred where it is smaller than the view, else moved by the offset but always
    /// covering the view.
    private func placeStage() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let s = shown, w = screen.width * s, h = screen.height * s
        var x = (bounds.width - w) / 2 + offset.x, y = (bounds.height - h) / 2 + offset.y
        x = w > bounds.width ? min(0, max(bounds.width - w, x)) : (bounds.width - w) / 2
        y = h > bounds.height ? min(0, max(bounds.height - h, y)) : (bounds.height - h) / 2
        offset = CGPoint(x: x - (bounds.width - w) / 2, y: y - (bounds.height - h) / 2)
        stage.bounds = CGRect(origin: .zero, size: screen)
        stage.transform = CATransform3DMakeScale(s, s, 1)
        stage.position = CGPoint(x: x, y: y)
        CATransaction.commit()
        placeArrow()
    }

    /// Two fingers pinching zoom in and out about the point between them, which stays under them (moving them
    /// moves the zoomed screen).
    private var pinchAnchor = CGPoint.zero
    @objc private func pinched(_ g: UIPinchGestureRecognizer) {
        let c = g.location(in: self)
        switch g.state {
        case .began:
            pinchAnchor = mac(c)
        case .changed:
            zoom = min(max(zoom * g.scale, 1), 4)
            g.scale = 1
            placeStage()
            let now = layer.convert(pinchAnchor, from: stage)
            offset.x += c.x - now.x
            offset.y += c.y - now.y
            placeStage()
        case .ended, .cancelled:
            if zoom < 1.05 {
                zoom = 1
                offset = .zero
                placeStage()
            }
        default: break
        }
    }

    /// Zoomed in, the screen moves to keep the pointer in view.
    private func followPointer() {
        guard zoom > 1 else { return }
        let v = layer.convert(cursor, from: stage), m: CGFloat = 36
        var d = CGPoint.zero
        if v.x < m { d.x = m - v.x } else if v.x > bounds.width - m { d.x = bounds.width - m - v.x }
        if v.y < m { d.y = m - v.y } else if v.y > bounds.height - m { d.y = bounds.height - m - v.y }
        guard d != .zero else { return }
        offset.x += d.x
        offset.y += d.y
        placeStage()
    }

    /// Puts every window the app shows at its place, back to front.
    func refresh() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        holders.forEach { $0.removeFromSuperlayer() }
        holders.removeAll()
        let count = Int(henge_tl_app_window_count())
        for i in 0..<count {
            guard let raw = henge_tl_app_window_layer(Int32(i)) else { continue }
            let root = Unmanaged<CALayer>.fromOpaque(raw).takeUnretainedValue()
            var x = 0.0, y = 0.0, w = 0.0, h = 0.0
            guard henge_tl_app_window_frame(Int32(i), &x, &y, &w, &h) == 0 else { continue }
            let holder = CALayer()
            holder.anchorPoint = .zero
            holder.frame = CGRect(x: x, y: y, width: w, height: h)
            root.removeFromSuperlayer()
            holder.addSublayer(root)
            stage.addSublayer(holder)
            holders.append(holder)
        }
        CATransaction.commit()
    }

    /// A point in this view to the Mac's screen (points, origin at the bottom left).
    private func mac(_ p: CGPoint) -> CGPoint { stage.convert(p, from: layer) }

    private func mouse(_ type: Int32, _ p: CGPoint, clicks: Int32 = 1, flags: UInt = 0) {
        moveCursor(to: mac(p))
        henge_tl_app_mouse(type, cursor.x, cursor.y, clicks, flags)
    }

    @objc private func tapped(_ g: UITapGestureRecognizer) {
        let p = g.location(in: self)
        mouse(5, p, clicks: 0)
        mouse(1, p)
        mouse(2, p)
    }

    /// The keyboard button: the iOS keyboard comes up as the Mac's keyboard, or goes (the screen keeps the hardware
    /// keyboard either way).
    private var softKeyboard = false
    private let noKeyboard = UIView(frame: .zero)
    override var inputView: UIView? { softKeyboard ? nil : noKeyboard }
    func toggleKeyboard() {
        softKeyboard.toggle()
        if !isFirstResponder { _ = becomeFirstResponder() }
        reloadInputViews()
    }

    /// Above the iOS keyboard, the Mac's keys it lacks: esc, tab, the modifiers (held for the next key) and arrows.
    let sticky = MacStickyKeys()
    private lazy var keyRow: UIHostingController<MacKeyRow> = {
        let host = UIHostingController(rootView: MacKeyRow(sticky: sticky, press: { [weak self] code, chars in
            self?.typeSpecial(code, chars)
        }, hide: { [weak self] in self?.toggleKeyboard() }))
        host.sizingOptions = []
        host.view.backgroundColor = .clear
        host.view.frame = CGRect(x: 0, y: 0, width: UIScreen.main.bounds.width, height: 50)
        host.view.autoresizingMask = .flexibleWidth
        return host
    }()
    override var inputAccessoryView: UIView? { softKeyboard ? keyRow.view : nil }

    /// A key of the row (esc, tab, an arrow), with the modifiers held for it.
    func typeSpecial(_ code: UInt16, _ chars: String) {
        key(code, chars, flags: sticky.flags)
        sticky.clear()
    }

    @objc private func doubleTapped(_ g: UITapGestureRecognizer) {
        let p = g.location(in: self)
        mouse(1, p, clicks: 1)
        mouse(2, p, clicks: 1)
        mouse(1, p, clicks: 2)
        mouse(2, p, clicks: 2)
    }

    @objc private func panned(_ g: UIPanGestureRecognizer) {
        let p = g.location(in: self)
        switch g.state {
        case .began: mouse(1, p)
        case .changed: mouse(6, p)
        case .ended, .cancelled, .failed: mouse(2, p)
        default: break
        }
    }

    private var lastScroll = CGPoint.zero
    @objc private func scrolled(_ g: UIPanGestureRecognizer) {
        let t = g.translation(in: self), p = mac(g.location(in: self))
        let phase: Int32 = g.state == .began ? 1 : (g.state == .ended || g.state == .cancelled) ? 8 : 4
        if g.state == .began { lastScroll = .zero }
        let s = max(shown, 0.01)
        henge_tl_app_scroll(p.x, p.y, (t.x - lastScroll.x) / s, (t.y - lastScroll.y) / s, phase)
        lastScroll = t
    }

    @objc private func pressed(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began else { return }
        let p = g.location(in: self)
        mouse(3, p)
        mouse(4, p)
    }

    @objc private func hovered(_ g: UIHoverGestureRecognizer) {
        if g.state == .began || g.state == .changed {
            mouse(5, g.location(in: self), clicks: 0)
            arrow.isHidden = true // a mouse or trackpad shows its own pointer
        }
    }

    @objc private func dragged(_ g: UIPanGestureRecognizer) {
        let p = g.location(in: self)
        switch g.state {
        case .began: mouse(1, p)
        case .changed: mouse(6, p)
        case .ended, .cancelled, .failed: mouse(2, p)
        default: break
        }
    }

    @objc private func rightClicked(_ g: UITapGestureRecognizer) {
        let p = g.location(in: self)
        mouse(3, p)
        mouse(4, p)
    }

    // MARK: the keyboard

    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        let held = sticky.flags
        for ch in text {
            var s = String(ch)
            if s == "\n" { key(36, "\r", flags: held); continue }
            let (code, shift) = MacKeys.code(for: ch)
            var flags = held | (shift ? 1 << 17 : 0)
            // With Command or Control held the key's plain character, as AppKit's key equivalents expect (⌘L, ⌃A);
            // Shift held from the row makes a letter a capital.
            if held & (1 << 20 | 1 << 18) != 0 { s = s.lowercased() }
            else if held & (1 << 17) != 0 { s = s.uppercased(); flags |= 1 << 17 }
            key(code, s, flags: flags)
        }
        sticky.clear()
    }

    func deleteBackward() {
        key(51, "\u{7f}", flags: sticky.flags)
        sticky.clear()
    }

    private func key(_ code: UInt16, _ chars: String, flags: UInt = 0) {
        chars.withCString { c in
            henge_tl_app_key(1, code, c, flags)
            henge_tl_app_key(0, code, c, flags)
        }
    }

    /// A hardware keyboard: every key with its Mac key code, held as long as it is held, with its modifiers (Command
    /// shortcuts included). Not passed on, so the text path (insertText) doesn't type it a second time.
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var rest = Set<UIPress>()
        for p in presses {
            guard let k = p.key, let code = MacKeys.hid(k.keyCode) else { rest.insert(p); continue }
            henge_tl_app_key(1, code.0, MacKeys.chars(k, code), MacKeys.flags(k.modifierFlags))
        }
        if !rest.isEmpty { super.pressesBegan(rest, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var rest = Set<UIPress>()
        for p in presses {
            guard let k = p.key, let code = MacKeys.hid(k.keyCode) else { rest.insert(p); continue }
            henge_tl_app_key(0, code.0, MacKeys.chars(k, code), MacKeys.flags(k.modifierFlags))
        }
        if !rest.isEmpty { super.pressesEnded(rest, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) { pressesEnded(presses, with: event) }
}

/// Modifiers tapped on the key row: held for the next key, then let go (AppKit's flag bits).
final class MacStickyKeys: ObservableObject {
    @Published private(set) var flags: UInt = 0
    func toggle(_ flag: UInt) { flags ^= flag }
    func clear() { if flags != 0 { flags = 0 } }
}

/// The row over the iOS keyboard on a Mac app's screen.
struct MacKeyRow: View {
    @ObservedObject var sticky: MacStickyKeys
    let press: (UInt16, String) -> Void
    let hide: () -> Void

    var body: some View {
        HStack(spacing: 5) {
            key("esc", 53, "\u{1b}")
            key("tab", 48, "\t")
            modifier("⌃", 1 << 18, "Control")
            modifier("⌥", 1 << 19, "Option")
            modifier("⇧", 1 << 17, "Shift")
            modifier("⌘", 1 << 20, "Command")
            key("←", 123, "\u{F702}")
            key("↑", 126, "\u{F700}")
            key("↓", 125, "\u{F701}")
            key("→", 124, "\u{F703}")
            Button(action: hide) {
                Image(systemName: "keyboard.chevron.compact.down")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Desk.text)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Desk.raised, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
            }
            .accessibilityLabel("Hide keyboard")
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 5)
        .frame(height: 50)
    }

    private func key(_ label: String, _ code: UInt16, _ chars: String) -> some View {
        Button {
            press(code, chars)
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        } label: {
            Text(label)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Desk.text)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Desk.raised, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
        }
        .accessibilityLabel(label)
    }

    private func modifier(_ symbol: String, _ flag: UInt, _ name: String) -> some View {
        let on = sticky.flags & flag != 0
        return Button {
            sticky.toggle(flag)
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        } label: {
            Text(symbol)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(on ? Color.white : Desk.text)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(on ? Desk.accent : Desk.raised, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
        }
        .accessibilityLabel(name)
    }
}

/// The Mac's key codes (Carbon's kVK_ values) for what the iOS keyboard types.
enum MacKeys {
    private static let ansi: [Character: UInt16] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11, "q": 12, "w": 13,
        "e": 14, "r": 15, "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "=": 24, "9": 25,
        "7": 26, "-": 27, "8": 28, "0": 29, "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37, "j": 38,
        "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44, "n": 45, "m": 46, ".": 47, "\t": 48, " ": 49, "`": 50,
    ]
    private static let shifted: [Character: Character] = [
        "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0", "_": "-",
        "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";", "\"": "'", "<": ",", ">": ".", "?": "/", "~": "`",
    ]

    static func code(for ch: Character) -> (UInt16, Bool) {
        if let c = ansi[ch] { return (c, false) }
        if ch.isUppercase, let lower = ch.lowercased().first, let c = ansi[lower] { return (c, true) }
        if let base = shifted[ch], let c = ansi[base] { return (c, true) }
        return (0, false)
    }

    /// Keys whose characters are not what they print: arrows, escape, return, delete, tab, function and page keys
    /// (AppKit's NSUpArrowFunctionKey and friends).
    static let special: [UIKeyboardHIDUsage: (UInt16, String)] = [
        .keyboardLeftArrow: (123, "\u{F702}"), .keyboardRightArrow: (124, "\u{F703}"), .keyboardDownArrow: (125, "\u{F701}"),
        .keyboardUpArrow: (126, "\u{F700}"), .keyboardEscape: (53, "\u{1b}"), .keyboardReturnOrEnter: (36, "\r"),
        .keypadEnter: (76, "\u{3}"), .keyboardDeleteOrBackspace: (51, "\u{7f}"), .keyboardTab: (48, "\t"),
        .keyboardDeleteForward: (117, "\u{F728}"), .keyboardHome: (115, "\u{F729}"), .keyboardEnd: (119, "\u{F72B}"),
        .keyboardPageUp: (116, "\u{F72C}"), .keyboardPageDown: (121, "\u{F72D}"),
        .keyboardF1: (122, "\u{F704}"), .keyboardF2: (120, "\u{F705}"), .keyboardF3: (99, "\u{F706}"), .keyboardF4: (118, "\u{F707}"),
        .keyboardF5: (96, "\u{F708}"), .keyboardF6: (97, "\u{F709}"), .keyboardF7: (98, "\u{F70A}"), .keyboardF8: (100, "\u{F70B}"),
        .keyboardF9: (101, "\u{F70C}"), .keyboardF10: (109, "\u{F70D}"), .keyboardF11: (103, "\u{F70E}"), .keyboardF12: (111, "\u{F70F}"),
    ]

    /// Printing keys by position (US layout positions, as the Mac's key codes are): letters, digits, punctuation, space.
    private static let printing: [UIKeyboardHIDUsage: UInt16] = [
        .keyboardA: 0, .keyboardS: 1, .keyboardD: 2, .keyboardF: 3, .keyboardH: 4, .keyboardG: 5, .keyboardZ: 6, .keyboardX: 7,
        .keyboardC: 8, .keyboardV: 9, .keyboardB: 11, .keyboardQ: 12, .keyboardW: 13, .keyboardE: 14, .keyboardR: 15,
        .keyboardY: 16, .keyboardT: 17, .keyboard1: 18, .keyboard2: 19, .keyboard3: 20, .keyboard4: 21, .keyboard6: 22,
        .keyboard5: 23, .keyboardEqualSign: 24, .keyboard9: 25, .keyboard7: 26, .keyboardHyphen: 27, .keyboard8: 28,
        .keyboard0: 29, .keyboardCloseBracket: 30, .keyboardO: 31, .keyboardU: 32, .keyboardOpenBracket: 33, .keyboardI: 34,
        .keyboardP: 35, .keyboardL: 37, .keyboardJ: 38, .keyboardQuote: 39, .keyboardK: 40, .keyboardSemicolon: 41,
        .keyboardBackslash: 42, .keyboardComma: 43, .keyboardSlash: 44, .keyboardN: 45, .keyboardM: 46, .keyboardPeriod: 47,
        .keyboardSpacebar: 49, .keyboardGraveAccentAndTilde: 50,
    ]

    /// A hardware key's Mac key code and fixed characters (empty: the key's own), or nil for keys not passed on
    /// (modifiers alone, media keys).
    static func hid(_ usage: UIKeyboardHIDUsage) -> (UInt16, String)? {
        if let s = special[usage] { return s }
        if let c = printing[usage] { return (c, "") }
        return nil
    }

    static func chars(_ k: UIKey, _ code: (UInt16, String)) -> String {
        if !code.1.isEmpty { return code.1 }
        // With Command (or Control) held, the plain character, as AppKit's key equivalents expect.
        if k.modifierFlags.contains(.command) || k.modifierFlags.contains(.control) { return k.charactersIgnoringModifiers }
        return k.characters
    }

    static func flags(_ m: UIKeyModifierFlags) -> UInt {
        var f: UInt = 0
        if m.contains(.shift) { f |= 1 << 17 }
        if m.contains(.control) { f |= 1 << 18 }
        if m.contains(.alternate) { f |= 1 << 19 }
        if m.contains(.command) { f |= 1 << 20 }
        return f
    }
}

struct MacScreenView: UIViewRepresentable {
    let screen: CGSize
    func makeUIView(context: Context) -> MacScreenUIView {
        let v = MacScreenUIView()
        v.screen = screen
        MacScreenHub.shared.view = v
        return v
    }
    func updateUIView(_ v: MacScreenUIView, context: Context) { v.refresh() }
}

/// The app's menu bar menus (a Mac app's menu bar has no place on the phone), as an iOS menu: built afresh each time
/// it opens, so items are enabled, ticked and filled in as the app has them then.
struct MacMenuButton: UIViewRepresentable {
    func makeUIView(context: Context) -> UIButton {
        var c = UIButton.Configuration.plain()
        c.image = UIImage(systemName: "menubar.rectangle", withConfiguration: UIImage.SymbolConfiguration(pointSize: 15, weight: .medium))
        c.baseForegroundColor = UIColor.white.withAlphaComponent(0.9)
        c.background.backgroundColor = UIColor.black.withAlphaComponent(0.45)
        c.background.cornerRadius = 19
        c.contentInsets = .zero
        let b = UIButton(configuration: c)
        b.showsMenuAsPrimaryAction = true
        b.menu = UIMenu(children: [UIDeferredMenuElement.uncached { done in done(MacMenuButton.elements()) }])
        return b
    }

    func updateUIView(_ b: UIButton, context: Context) {}

    static func elements() -> [UIMenuElement] {
        guard let raw = henge_tl_app_main_menu(),
              let items = Unmanaged<NSArray>.fromOpaque(raw).takeRetainedValue() as? [[String: Any]], !items.isEmpty else {
            return [UIAction(title: "This app has no menus", attributes: .disabled) { _ in }]
        }
        return build(items)
    }

    /// Items as iOS menu elements; a Mac menu's separators become iOS's inline groups.
    private static func build(_ items: [[String: Any]]) -> [UIMenuElement] {
        var groups: [[UIMenuElement]] = [[]]
        for it in items {
            if it["separator"] as? Bool == true {
                groups.append([])
                continue
            }
            let title = it["title"] as? String ?? ""
            let path = (it["path"] as? [NSNumber] ?? []).map { Int32(truncating: $0) }
            if let kids = it["children"] as? [[String: Any]] {
                groups[groups.count - 1].append(UIMenu(title: title, children: build(kids)))
                continue
            }
            let enabled = it["enabled"] as? Bool ?? true
            let state = it["state"] as? Int ?? 0
            let a = UIAction(title: title, attributes: enabled ? [] : .disabled,
                             state: state > 0 ? .on : state < 0 ? .mixed : .off) { _ in
                path.withUnsafeBufferPointer { henge_tl_app_menu_choose($0.baseAddress, Int32(path.count)) }
            }
            groups[groups.count - 1].append(a)
        }
        let filled = groups.filter { !$0.isEmpty }
        if filled.count == 1 { return filled[0] }
        return filled.map { UIMenu(title: "", options: .displayInline, children: $0) }
    }
}

/// Where Henge's AppKit's "a window changed" lands (a C callback, on the main thread).
final class MacScreenHub {
    static let shared = MacScreenHub()
    weak var view: MacScreenUIView?
}

/// The full-screen place a windowed Mac app runs in: loads it, enters its main, shows its windows.
struct MacAppScreen: View {
    let app: MacApp
    @Environment(\.dismiss) private var dismiss
    @State private var status = "Loading…"
    @State private var started = false
    @State private var trackpad = UserDefaults.standard.object(forKey: MacScreenUIView.trackpadKey) as? Bool ?? true
    private let screen = MacAppScreen.screenSize()

    /// The Mac's screen in points: the device's long side by short side, made a little larger than the device so
    /// a desktop app has room (it is shown scaled down to fit).
    static func screenSize() -> CGSize {
        let b = UIScreen.main.bounds.size
        let long = max(b.width, b.height), short = min(b.width, b.height)
        let zoom: CGFloat = UIDevice.current.userInterfaceIdiom == .pad ? 1.0 : 1.35
        return CGSize(width: (long * zoom).rounded(), height: (short * zoom).rounded())
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            MacScreenView(screen: screen).ignoresSafeArea()
            HStack(spacing: 10) {
                // Trackpad (fingers move the pointer) or touch (tap where to click).
                Button {
                    trackpad.toggle()
                    MacScreenHub.shared.view?.trackpad = trackpad
                } label: {
                    Image(systemName: trackpad ? "cursorarrow" : "hand.point.up.left")
                        .font(.system(size: 15, weight: .medium)).foregroundStyle(.white.opacity(0.9))
                        .frame(width: 38, height: 38).background(.black.opacity(0.45), in: Circle())
                }
                MacMenuButton().frame(width: 38, height: 38)
                Button { MacScreenHub.shared.view?.toggleKeyboard() } label: {
                    Image(systemName: "keyboard").font(.system(size: 15, weight: .medium)).foregroundStyle(.white.opacity(0.9))
                        .frame(width: 38, height: 38).background(.black.opacity(0.45), in: Circle())
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing).padding(14)
            if !started || !status.isEmpty {
                Text(status).font(.system(size: 13, weight: .medium)).foregroundStyle(.white.opacity(0.85))
                    .padding(.horizontal, 12).padding(.vertical, 8)
                    .background(.black.opacity(0.55), in: Capsule()).padding(16)
            }
        }
        .statusBarHidden()
        .task { await start() }
    }

    private func start() async {
        guard !started else { return }
        started = true
        // 2x at most: the Mac's screen is shown scaled down to fit, and the app draws in software
        henge_tl_app_set_screen(screen.width, screen.height, min(UIScreen.main.scale, 2))
        henge_tl_app_on_change { MacScreenHub.shared.view?.refresh() }
        let path = app.url.path
        HengeLog.log("macapps", "opening \(app.name) with windows (\(path))")
        let failure: String? = await Task.detached(priority: .userInitiated) {
            var err = [CChar](repeating: 0, count: 512)
            return henge_tl_app_load(path, &err, err.count) == 0 ? nil : String(cString: err)
        }.value
        if let failure {
            HengeLog.log("macapps", "could not load \(app.name): \(failure)")
            status = "Could not open it: \(failure)"
            return
        }
        status = "Starting…"
        henge_tl_app_run_soon { rc in
            HengeLog.log("macapps", "the app's main returned \(rc)")
        }
        try? await Task.sleep(nanoseconds: 4_000_000_000)
        status = ""
    }
}
