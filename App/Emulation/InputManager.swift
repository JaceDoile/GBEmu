import GameController

/// Game controllers (Xbox, PlayStation, Switch, MFi, Backbone/Kishi…) and hardware keyboards.
final class InputManager {
    var onPadButtons: ((UInt8) -> Void)?
    var onKeyButtons: ((UInt8) -> Void)?
    var onFastForward: ((Bool) -> Void)?
    var onMenu: (() -> Void)?
    var onControllerChange: ((Bool) -> Void)?

    private var observers: [NSObjectProtocol] = []
    private var keysDown: Set<GCKeyCode> = []
    private var padFF = false, keyFF = false
    private var lastPad: UInt8 = 0
    private var leftTriggerWasDown = false

    var controllerConnected: Bool { GCController.controllers().contains { $0.extendedGamepad != nil || $0.microGamepad != nil } }

    init() {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.setup(c) }
            self?.onControllerChange?(self?.controllerConnected ?? false)
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.lastPad = 0
            self?.onPadButtons?(0)
            self?.onControllerChange?(self?.controllerConnected ?? false)
        })
        observers.append(nc.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] n in
            if let k = n.object as? GCKeyboard { self?.setupKeyboard(k) }
        })
        GCController.controllers().forEach(setup)
        if let k = GCKeyboard.coalesced { setupKeyboard(k) }
        GCController.startWirelessControllerDiscovery(completionHandler: nil)
    }

    deinit {
        observers.forEach { NotificationCenter.default.removeObserver($0) }
        GCController.stopWirelessControllerDiscovery()
        for c in GCController.controllers() {
            c.extendedGamepad?.valueChangedHandler = nil
            c.microGamepad?.valueChangedHandler = nil
        }
        GCKeyboard.coalesced?.keyboardInput?.keyChangedHandler = nil
    }

    private func setup(_ c: GCController) {
        c.handlerQueue = .main
        if let pad = c.extendedGamepad {
            pad.valueChangedHandler = { [weak self] pad, _ in self?.read(pad) }
        } else if let micro = c.microGamepad {
            micro.reportsAbsoluteDpadValues = false
            micro.allowsRotation = true
            micro.valueChangedHandler = { [weak self] m, _ in self?.readMicro(m) }
        }
    }

    private func read(_ p: GCExtendedGamepad) {
        var m: UInt8 = 0
        let byPosition = AppSettings.shared.controllerByPosition
        // Face buttons. GameController names them by Xbox position:
        // buttonA = bottom, buttonB = right, buttonX = left, buttonY = top.
        let aPressed: Bool, bPressed: Bool
        if byPosition {
            // Game Boy layout: A is the right button, B the lower/left one.
            aPressed = p.buttonB.isPressed || p.buttonY.isPressed
            bPressed = p.buttonA.isPressed || p.buttonX.isPressed
        } else {
            aPressed = p.buttonA.isPressed || p.buttonY.isPressed
            bPressed = p.buttonB.isPressed || p.buttonX.isPressed
        }
        if aPressed { m |= Btn.a }
        if bPressed { m |= Btn.b }
        if p.buttonMenu.isPressed { m |= Btn.start }
        if p.buttonOptions?.isPressed == true { m |= Btn.select }

        let dx = p.dpad.xAxis.value + p.leftThumbstick.xAxis.value
        let dy = p.dpad.yAxis.value + p.leftThumbstick.yAxis.value
        if dx > 0.5 { m |= Btn.right } else if dx < -0.5 { m |= Btn.left }
        if dy > 0.5 { m |= Btn.up } else if dy < -0.5 { m |= Btn.down }

        if m != lastPad { lastPad = m; onPadButtons?(m) }

        let ff = p.rightShoulder.isPressed || p.rightTrigger.isPressed
        if ff != padFF { padFF = ff; onFastForward?(ff) }

        let lt = p.leftTrigger.isPressed || p.leftShoulder.isPressed
        if lt && !leftTriggerWasDown { onMenu?() }
        leftTriggerWasDown = lt
    }

    private func readMicro(_ p: GCMicroGamepad) {
        var m: UInt8 = 0
        if p.buttonA.isPressed { m |= Btn.a }
        if p.buttonX.isPressed { m |= Btn.b }
        if p.buttonMenu.isPressed { m |= Btn.start }
        if p.dpad.xAxis.value > 0.5 { m |= Btn.right } else if p.dpad.xAxis.value < -0.5 { m |= Btn.left }
        if p.dpad.yAxis.value > 0.5 { m |= Btn.up } else if p.dpad.yAxis.value < -0.5 { m |= Btn.down }
        if m != lastPad { lastPad = m; onPadButtons?(m) }
    }

    // MARK: Keyboard
    // Arrows / WASD = D-pad, X or K = A, Z or J = B, Return = Start,
    // Shift / Backspace = Select, Space (hold) = fast forward, Esc / Tab = menu.

    private func setupKeyboard(_ k: GCKeyboard) {
        k.handlerQueue = .main
        k.keyboardInput?.keyChangedHandler = { [weak self] _, _, key, pressed in
            self?.key(key, pressed)
        }
    }

    private func key(_ key: GCKeyCode, _ pressed: Bool) {
        if pressed { keysDown.insert(key) } else { keysDown.remove(key) }
        if pressed && (key == .escape || key == .tab) { onMenu?(); return }

        func any(_ keys: [GCKeyCode]) -> Bool { keys.contains { keysDown.contains($0) } }
        var m: UInt8 = 0
        if any([.rightArrow, .keyD]) { m |= Btn.right }
        if any([.leftArrow, .keyA]) { m |= Btn.left }
        if any([.upArrow, .keyW]) { m |= Btn.up }
        if any([.downArrow, .keyS]) { m |= Btn.down }
        if any([.keyX, .keyK]) { m |= Btn.a }
        if any([.keyZ, .keyJ]) { m |= Btn.b }
        if any([.returnOrEnter, .keypadEnter]) { m |= Btn.start }
        if any([.leftShift, .rightShift, .deleteOrBackspace]) { m |= Btn.select }
        onKeyButtons?(m)

        let ff = keysDown.contains(.spacebar)
        if ff != keyFF { keyFF = ff; onFastForward?(ff) }
    }
}
