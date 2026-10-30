import UIKit

/// On-screen controls with raw multi-touch handling.
///  - The D-pad keeps ownership of a finger even if it drifts off, so you never "lose" the pad.
///  - Fingers on the face buttons can slide between A and B; the gap between them presses both.
///  - Cardinal directions get 60° sectors, diagonals 30°, so grid-based games don't slip diagonally.
final class ControlsView: UIView {
    var onButtons: ((UInt8) -> Void)?
    var onMenu: (() -> Void)?
    var onFastForward: ((Bool) -> Void)?   // toggle mode: new state; hold mode: pressed/released

    var fastForwardOn = false { didSet { if fastForwardOn != oldValue { setNeedsDisplay() } } }
    var fastForwardHold = false
    var haptics = true
    /// Area the game screen occupies, used to lay controls out around it.
    var screenFrame: CGRect = .zero { didSet { if screenFrame != oldValue { setNeedsLayout(); setNeedsDisplay() } } }
    /// Only menu + fast-forward are shown (a controller is connected).
    var minimal = false { didSet { if minimal != oldValue { releaseAll(); setNeedsDisplay() } } }

    private enum Owner { case dpad, buttons, menu, ff }

    private var dpadCenter = CGPoint.zero, dpadRadius: CGFloat = 70
    private var aCenter = CGPoint.zero, bCenter = CGPoint.zero, btnRadius: CGFloat = 32
    private var startRect = CGRect.zero, selectRect = CGRect.zero
    private var menuRect = CGRect.zero, ffRect = CGRect.zero

    private var owners: [UITouch: Owner] = [:]
    private var masks: [UITouch: UInt8] = [:]
    private var mask: UInt8 = 0
    private let impact = UIImpactFeedbackGenerator(style: .light)
    private let impactHeavy = UIImpactFeedbackGenerator(style: .medium)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
    }
    required init?(coder: NSCoder) { fatalError() }

    // MARK: Layout

    override func layoutSubviews() {
        super.layoutSubviews()
        let b = bounds, s = safeAreaInsets
        let landscape = b.width > b.height
        if landscape {
            let leftW = max(screenFrame.minX, 1), rightW = max(b.width - screenFrame.maxX, 1)
            let side = min(leftW, rightW)
            dpadRadius = min(max(side * 0.36, 58), 80, b.height * 0.2)
            btnRadius = dpadRadius * 0.46
            let cy = b.height * 0.56
            let dx = max(s.left + dpadRadius + 18, leftW / 2 + s.left / 2)
            dpadCenter = CGPoint(x: dx, y: cy)
            let bx = min(b.width - s.right - btnRadius * 2.6, b.width - rightW / 2 - s.right / 2)
            aCenter = CGPoint(x: bx + btnRadius * 1.25, y: cy - btnRadius * 0.7)
            bCenter = CGPoint(x: bx - btnRadius * 1.25, y: cy + btnRadius * 0.7)
            let pill = CGSize(width: 58, height: 22)
            let py = min(b.height - s.bottom - pill.height - 14, cy + dpadRadius + 46)
            selectRect = CGRect(x: dpadCenter.x - pill.width / 2, y: py, width: pill.width, height: pill.height)
            startRect = CGRect(x: (aCenter.x + bCenter.x) / 2 - pill.width / 2, y: py, width: pill.width, height: pill.height)
            let top = s.top + 14
            menuRect = CGRect(x: s.left + 16, y: top, width: 58, height: 30)
            ffRect = CGRect(x: b.width - s.right - 16 - 58, y: top, width: 58, height: 30)
        } else {
            let top = screenFrame.maxY + 8
            let bottom = b.height - max(s.bottom, 12)
            let h = max(bottom - top, 200)
            menuRect = CGRect(x: 16, y: top + 6, width: 58, height: 30)
            ffRect = CGRect(x: b.width - 16 - 58, y: top + 6, width: 58, height: 30)
            dpadRadius = min(b.width * 0.2, h * 0.27, 90)
            btnRadius = dpadRadius * 0.46
            let cy = top + 46 + (h - 46) * 0.40
            dpadCenter = CGPoint(x: max(b.width * 0.26, dpadRadius + 22), y: cy)
            let bx = min(b.width * 0.74, b.width - btnRadius * 2.6 - 10)
            aCenter = CGPoint(x: bx + btnRadius * 1.25, y: cy - btnRadius * 0.7)
            bCenter = CGPoint(x: bx - btnRadius * 1.25, y: cy + btnRadius * 0.7)
            let pill = CGSize(width: 62, height: 22)
            let py = min(bottom - pill.height - 14, cy + dpadRadius + 34)
            selectRect = CGRect(x: b.width / 2 - pill.width - 12, y: py, width: pill.width, height: pill.height)
            startRect = CGRect(x: b.width / 2 + 12, y: py, width: pill.width, height: pill.height)
        }
        setNeedsDisplay()
    }

    // MARK: Hit testing

    private func dist(_ a: CGPoint, _ b: CGPoint) -> CGFloat { hypot(a.x - b.x, a.y - b.y) }

    private func dpadMask(_ p: CGPoint) -> UInt8 {
        let dx = p.x - dpadCenter.x, dy = p.y - dpadCenter.y
        if hypot(dx, dy) < dpadRadius * 0.16 { return 0 }
        let a = atan2(dy, dx) * 180 / .pi     // 0 = right, 90 = down
        switch a {
        case -30...30: return Btn.right
        case 30..<60: return Btn.right | Btn.down
        case 60...120: return Btn.down
        case 120..<150: return Btn.left | Btn.down
        case -60 ..< -30: return Btn.right | Btn.up
        case -120 ... -60: return Btn.up
        case -150 ..< -120: return Btn.left | Btn.up
        default: return Btn.left
        }
    }

    private func buttonMask(_ p: CGPoint) -> UInt8 {
        var m: UInt8 = 0
        let reach = btnRadius * 1.45
        if dist(p, aCenter) <= reach { m |= Btn.a }
        if dist(p, bCenter) <= reach { m |= Btn.b }
        if m == 0 {
            // Nearest face button for touches just outside the circles
            let da = dist(p, aCenter), db = dist(p, bCenter)
            if min(da, db) <= btnRadius * 1.9 { m = da < db ? Btn.a : Btn.b }
        }
        if startRect.insetBy(dx: -16, dy: -14).contains(p) { m |= Btn.start }
        if selectRect.insetBy(dx: -16, dy: -14).contains(p) { m |= Btn.select }
        return m
    }

    private func owner(at p: CGPoint) -> Owner? {
        if menuRect.insetBy(dx: -12, dy: -12).contains(p) { return .menu }
        if ffRect.insetBy(dx: -12, dy: -12).contains(p) { return .ff }
        if minimal { return nil }
        if dist(p, dpadCenter) <= dpadRadius * 1.35 { return .dpad }
        if buttonMask(p) != 0 { return .buttons }
        // Anywhere else on the left half is treated as the D-pad, so a slightly-off thumb still works.
        if p.x < bounds.width * 0.42 && p.y > (bounds.width > bounds.height ? 0 : screenFrame.maxY) { return .dpad }
        return nil
    }

    // MARK: Touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let p = t.location(in: self)
            guard let o = owner(at: p) else { continue }
            owners[t] = o
            switch o {
            case .dpad: masks[t] = dpadMask(p)
            case .buttons: masks[t] = buttonMask(p)
            case .ff:
                if fastForwardHold { fastForwardOn = true; onFastForward?(true) }
                else { fastForwardOn.toggle(); onFastForward?(fastForwardOn) }
                if haptics { impactHeavy.impactOccurred() }
            case .menu: break
            }
        }
        update()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            guard let o = owners[t] else { continue }
            let p = t.location(in: self)
            switch o {
            case .dpad: masks[t] = dpadMask(p)
            case .buttons: masks[t] = buttonMask(p)
            default: break
            }
        }
        update()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let o = owners.removeValue(forKey: t)
            masks.removeValue(forKey: t)
            if o == .menu, menuRect.insetBy(dx: -24, dy: -24).contains(t.location(in: self)) { onMenu?() }
            if o == .ff, fastForwardHold { fastForwardOn = false; onFastForward?(false) }
        }
        update()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let o = owners.removeValue(forKey: t)
            masks.removeValue(forKey: t)
            if o == .ff, fastForwardHold { fastForwardOn = false; onFastForward?(false) }
        }
        update()
    }

    func releaseAll() {
        owners.removeAll()
        masks.removeAll()
        update()
    }

    private func update() {
        let m = masks.values.reduce(UInt8(0)) { $0 | $1 }
        guard m != mask else { return }
        let newlyPressed = m & ~mask
        mask = m
        if haptics && newlyPressed != 0 { impact.impactOccurred(intensity: 0.7) }
        onButtons?(m)
        setNeedsDisplay()
    }

    // MARK: Drawing

    override func draw(_ rect: CGRect) {
        let fill = UIColor(white: 1, alpha: 0.16), fillOn = UIColor(white: 1, alpha: 0.45)
        let stroke = UIColor(white: 1, alpha: 0.35), text = UIColor(white: 1, alpha: 0.85)

        func label(_ s: String, _ r: CGRect, size: CGFloat, weight: UIFont.Weight = .bold) {
            let attrs: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: size, weight: weight), .foregroundColor: text]
            let sz = (s as NSString).size(withAttributes: attrs)
            (s as NSString).draw(at: CGPoint(x: r.midX - sz.width / 2, y: r.midY - sz.height / 2), withAttributes: attrs)
        }
        func pill(_ r: CGRect, on: Bool) {
            let p = UIBezierPath(roundedRect: r, cornerRadius: r.height / 2)
            (on ? fillOn : fill).setFill(); p.fill()
            stroke.setStroke(); p.lineWidth = 1; p.stroke()
        }

        pill(menuRect, on: owners.values.contains(.menu))
        label("MENU", menuRect, size: 11)
        pill(ffRect, on: fastForwardOn)
        label(fastForwardOn ? "▶▶ ON" : "▶▶", ffRect, size: 11)

        if minimal { return }

        // D-pad: a cross
        let arm = dpadRadius * 0.36, len = dpadRadius * 0.92
        let c = dpadCenter
        let horiz = CGRect(x: c.x - len, y: c.y - arm, width: len * 2, height: arm * 2)
        let vert = CGRect(x: c.x - arm, y: c.y - len, width: arm * 2, height: len * 2)
        let cross = UIBezierPath(roundedRect: horiz, cornerRadius: arm * 0.35)
        cross.append(UIBezierPath(roundedRect: vert, cornerRadius: arm * 0.35))
        fill.setFill(); cross.fill()
        stroke.setStroke()
        UIBezierPath(roundedRect: horiz, cornerRadius: arm * 0.35).stroke()
        UIBezierPath(roundedRect: vert, cornerRadius: arm * 0.35).stroke()
        let seg = len - arm
        fillOn.setFill()
        if mask & Btn.up != 0 { UIBezierPath(roundedRect: CGRect(x: c.x - arm, y: c.y - len, width: arm * 2, height: seg), cornerRadius: 4).fill() }
        if mask & Btn.down != 0 { UIBezierPath(roundedRect: CGRect(x: c.x - arm, y: c.y + arm, width: arm * 2, height: seg), cornerRadius: 4).fill() }
        if mask & Btn.left != 0 { UIBezierPath(roundedRect: CGRect(x: c.x - len, y: c.y - arm, width: seg, height: arm * 2), cornerRadius: 4).fill() }
        if mask & Btn.right != 0 { UIBezierPath(roundedRect: CGRect(x: c.x + arm, y: c.y - arm, width: seg, height: arm * 2), cornerRadius: 4).fill() }

        // A / B
        for (center, bit, name) in [(aCenter, Btn.a, "A"), (bCenter, Btn.b, "B")] {
            let r = CGRect(x: center.x - btnRadius, y: center.y - btnRadius, width: btnRadius * 2, height: btnRadius * 2)
            let p = UIBezierPath(ovalIn: r)
            (mask & bit != 0 ? fillOn : fill).setFill(); p.fill()
            stroke.setStroke(); p.lineWidth = 1.5; p.stroke()
            label(name, r, size: btnRadius * 0.7)
        }

        // Start / Select
        pill(selectRect, on: mask & Btn.select != 0)
        pill(startRect, on: mask & Btn.start != 0)
        label("SELECT", selectRect.offsetBy(dx: 0, dy: selectRect.height + 2).insetBy(dx: -20, dy: 0), size: 9, weight: .semibold)
        label("START", startRect.offsetBy(dx: 0, dy: startRect.height + 2).insetBy(dx: -20, dy: 0), size: 9, weight: .semibold)
    }
}
