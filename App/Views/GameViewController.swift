import UIKit
import SwiftUI
import Combine

/// Full-screen game view: screen + touch controls + controller input + pause menu.
final class GameViewController: UIViewController, UIAdaptivePresentationControllerDelegate {
    let emulator: Emulator
    private let screen = UIView()
    private let controls = ControlsView()
    private let toast = UILabel()
    private let input = InputManager()
    private let settings = AppSettings.shared
    private var cancellables: Set<AnyCancellable> = []
    private var menuOpen = false
    private var touchFF = false, padFF = false, keyFF = false
    var onClose: (() -> Void)?

    init(emulator: Emulator) {
        self.emulator = emulator
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .fullScreen
    }
    required init?(coder: NSCoder) { fatalError() }

    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    // Stops swipes from the screen edges (Control Center etc.) stealing D-pad/button touches.
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.07, green: 0.07, blue: 0.08, alpha: 1)

        screen.backgroundColor = .black
        screen.layer.contentsGravity = .resize
        screen.layer.contents = emulator.lastImage
        view.addSubview(screen)

        controls.frame = view.bounds
        controls.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(controls)

        toast.font = .systemFont(ofSize: 14, weight: .semibold)
        toast.textColor = .white
        toast.backgroundColor = UIColor(white: 0, alpha: 0.7)
        toast.textAlignment = .center
        toast.layer.cornerRadius = 10
        toast.layer.masksToBounds = true
        toast.alpha = 0
        view.addSubview(toast)

        emulator.onFrame = { [weak self] img in self?.screen.layer.contents = img }

        controls.onButtons = { [weak self] m in self?.emulator.touchButtons = m }
        controls.onMenu = { [weak self] in self?.showMenu() }
        controls.onFastForward = { [weak self] on in self?.touchFF = on; self?.updateFF() }

        input.onPadButtons = { [weak self] m in self?.emulator.padButtons = m }
        input.onKeyButtons = { [weak self] m in self?.emulator.keyButtons = m }
        input.onFastForward = { [weak self] on in
            guard let self else { return }
            // shared by pad and keyboard; either holding counts
            self.padFF = on
            self.updateFF()
        }
        input.onMenu = { [weak self] in
            guard let self else { return }
            if self.menuOpen { self.dismissMenu() } else { self.showMenu() }
        }
        input.onControllerChange = { [weak self] _ in self?.applySettings() }

        settings.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in DispatchQueue.main.async { self?.applySettings() } }
            .store(in: &cancellables)
        applySettings()

        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(willResignActive), name: UIApplication.willResignActiveNotification, object: nil)
        nc.addObserver(self, selector: #selector(didBecomeActive), name: UIApplication.didBecomeActiveNotification, object: nil)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        // System edge gestures otherwise delay touches near the screen edges.
        view.window?.gestureRecognizers?.forEach { $0.delaysTouchesBegan = false }
        setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        emulator.start()
    }

    private func applySettings() {
        controls.alpha = CGFloat(settings.controlsOpacity)
        controls.haptics = settings.haptics
        controls.fastForwardHold = settings.fastForwardHold
        controls.minimal = settings.hideTouchWithController && input.controllerConnected
        let filter: CALayerContentsFilter = settings.smoothing ? .linear : .nearest
        screen.layer.magnificationFilter = filter
        screen.layer.minificationFilter = filter
        view.setNeedsLayout()
    }

    private func updateFF() {
        let on = touchFF || padFF || keyFF
        emulator.fastForward = on
        if controls.fastForwardOn != touchFF { controls.fastForwardOn = touchFF }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let b = view.bounds, s = view.safeAreaInsets
        let landscape = b.width > b.height
        var avail: CGRect
        if landscape {
            avail = CGRect(x: s.left, y: s.top, width: b.width - s.left - s.right, height: b.height - s.top - s.bottom)
        } else {
            let h = min(b.width * 0.9, (b.height - s.top) * 0.52)
            avail = CGRect(x: 0, y: s.top + 4, width: b.width, height: h)
        }
        var scale = min(avail.width / 160, avail.height / 144)
        if settings.integerScaling {
            let px = view.window?.screen.scale ?? UIScreen.main.scale
            let k = floor(scale * px)
            if k >= px { scale = k / px }
        }
        let size = CGSize(width: 160 * scale, height: 144 * scale)
        let x = avail.midX - size.width / 2
        let y = landscape ? avail.midY - size.height / 2 : avail.minY
        screen.frame = CGRect(x: x, y: y, width: size.width, height: size.height).integral
        controls.screenFrame = screen.frame
        toast.frame = CGRect(x: b.midX - 110, y: screen.frame.minY + 12, width: 220, height: 34)
    }

    // MARK: Lifecycle

    @objc private func willResignActive() {
        emulator.suspend()
    }

    @objc private func didBecomeActive() {
        if !menuOpen && presentedViewController == nil { emulator.resume() }
    }

    func showToast(_ text: String) {
        toast.text = text
        toast.layer.removeAllAnimations()
        UIView.animate(withDuration: 0.15) { self.toast.alpha = 1 }
        UIView.animate(withDuration: 0.3, delay: 1.4, options: []) { self.toast.alpha = 0 }
    }

    // MARK: Menu

    func showMenu() {
        guard !menuOpen else { return }
        menuOpen = true
        controls.releaseAll()
        emulator.pause()
        let model = GameMenuModel(emulator: emulator)
        model.close = { [weak self] message in
            self?.dismissMenu()
            if let message { self?.showToast(message) }
        }
        model.quit = { [weak self] in self?.quit() }
        let host = UIHostingController(rootView: GameMenuView(model: model).environmentObject(settings))
        host.modalPresentationStyle = .pageSheet
        if let sheet = host.sheetPresentationController {
            sheet.detents = [.large()]
            sheet.prefersGrabberVisible = true
        }
        host.presentationController?.delegate = self
        present(host, animated: true)
    }

    func dismissMenu() {
        guard menuOpen else { return }
        if presentedViewController != nil {
            dismiss(animated: true) { [weak self] in self?.menuClosed() }
        } else {
            menuClosed()
        }
    }

    private func menuClosed() {
        menuOpen = false
        applySettings()
        emulator.resume()
    }

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        menuClosed()
    }

    private func quit() {
        emulator.stop()
        menuOpen = false
        // Dismissing from the presenter closes both the menu sheet and the game.
        presentingViewController?.dismiss(animated: true)
        onClose?()
    }
}
