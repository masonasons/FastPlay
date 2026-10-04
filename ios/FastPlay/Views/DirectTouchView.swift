import UIKit

/// The player's touch mode: the whole area takes swipes.
///
///   one finger left or right     seek back or forward
///   one finger up or down        the next or previous seek unit
///                                (in spring and tape seeking: the speed)
///   one finger held and dragged  in spring and tape seeking: scrubs back (to the
///   left or right                left) or forward for as long as it is held
///   two fingers left or right    the previous or next effect parameter
///   two fingers up or down       the parameter up or down
///   two finger tap               play or pause
///
/// VoiceOver passes touches straight through while its cursor is on this view
/// (direct interaction), so the same swipes work with it running, and FastPlay
/// says what each one did.
final class DirectTouchView: UIView, UIGestureRecognizerDelegate {
    /// Spring or tape seeking: a drag left or right scrubs while it is held, in
    /// place of the swipe that jumps.
    var scrubMode = false {
        didSet {
            guard scrubMode != oldValue else { return }
            guide.text = guideText
            accessibilityHint = hintText
        }
    }

    /// A drag began (or turned round): scrub back (-1) or forward (1) until onScrubStop.
    var onScrubStart: ((Int) -> Void)?
    var onScrubStop: (() -> Void)?
    private var scrubDirection = 0
    private let guide = UILabel()
    private let drag = UIPanGestureRecognizer()

    var onSeek: ((Int) -> Void)?
    var onSeekUnit: ((Int) -> Void)?
    var onParam: ((Int) -> Void)?
    var onAdjust: ((Int) -> Void)?
    var onPlayPause: (() -> Void)?

    let seekLabel = UILabel()
    let effectLabel = UILabel()

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .secondarySystemBackground
        layer.cornerRadius = 16
        layer.borderWidth = 1
        layer.borderColor = UIColor.separator.cgColor

        guide.text = guideText
        guide.numberOfLines = 0
        guide.textAlignment = .center
        guide.textColor = .secondaryLabel
        guide.font = .preferredFont(forTextStyle: .footnote)
        guide.adjustsFontForContentSizeCategory = true

        for label in [seekLabel, effectLabel] {
            label.font = .preferredFont(forTextStyle: .title2)
            label.adjustsFontForContentSizeCategory = true
            label.textAlignment = .center
            label.numberOfLines = 2
        }

        let stack = UIStackView(arrangedSubviews: [seekLabel, effectLabel, guide])
        stack.axis = .vertical
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.isUserInteractionEnabled = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            stack.topAnchor.constraint(greaterThanOrEqualTo: topAnchor, constant: 12),
        ])

        for fingers in 1...2 {
            for direction: UISwipeGestureRecognizer.Direction in [.left, .right, .up, .down] {
                let swipe = UISwipeGestureRecognizer(target: self, action: #selector(swiped(_:)))
                swipe.direction = direction
                swipe.numberOfTouchesRequired = fingers
                swipe.delegate = self
                addGestureRecognizer(swipe)
            }
        }
        drag.maximumNumberOfTouches = 1
        drag.addTarget(self, action: #selector(dragged(_:)))
        drag.delegate = self
        addGestureRecognizer(drag)
        let tap = UITapGestureRecognizer(target: self, action: #selector(twoFingerTapped))
        tap.numberOfTouchesRequired = 2
        addGestureRecognizer(tap)

        isAccessibilityElement = true
        accessibilityTraits = .allowsDirectInteraction
        accessibilityLabel = "Touch controls"
        accessibilityHint = hintText
    }

    private var guideText: String {
        (scrubMode ? "Hold and drag left or right to scrub, swipe up or down for the speed.\n"
                   : "Swipe left or right to seek, up or down for the seek unit.\n") +
            "With two fingers: left or right for the effect, up or down to adjust it. Two finger tap: play or pause."
    }

    private var hintText: String {
        (scrubMode ? "Hold and drag left or right to scrub back or forward, and swipe up or down to change the speed. "
                   : "Swipe left or right to seek, up or down to change the seek unit. ") +
            "With two fingers, swipe left or right to choose an effect, up or down to adjust it, and tap to play or pause."
    }

    /// The drag is for scrubbing, and only when it sets off sideways; while it is,
    /// the one finger swipe left or right (a jump) stands aside.
    override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        if recognizer === drag {
            let velocity = drag.velocity(in: self)
            return scrubMode && abs(velocity.x) > abs(velocity.y)
        }
        if let swipe = recognizer as? UISwipeGestureRecognizer, swipe.numberOfTouchesRequired == 1,
           swipe.direction == .left || swipe.direction == .right {
            return !scrubMode
        }
        return true
    }

    @objc private func dragged(_ drag: UIPanGestureRecognizer) {
        switch drag.state {
        case .began, .changed:
            // Which side of where it started the finger is on; a little way past
            // the start before it turns round, so a wobble does not
            let offset = drag.translation(in: self).x
            var direction = scrubDirection
            if drag.state == .began { direction = drag.velocity(in: self).x < 0 ? -1 : 1 }
            if offset > 24 { direction = 1 } else if offset < -24 { direction = -1 }
            if direction != scrubDirection {
                scrubDirection = direction
                onScrubStart?(direction)
            }
        default:
            if scrubDirection != 0 {
                scrubDirection = 0
                onScrubStop?()
            }
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    @objc private func swiped(_ swipe: UISwipeGestureRecognizer) {
        let step = (swipe.direction == .right || swipe.direction == .up) ? 1 : -1
        let horizontal = swipe.direction == .left || swipe.direction == .right
        switch (swipe.numberOfTouchesRequired, horizontal) {
        case (1, true): onSeek?(step)
        case (1, false): onSeekUnit?(step)
        case (_, true): onParam?(step)
        case (_, false): onAdjust?(step)
        }
    }

    @objc private func twoFingerTapped() {
        onPlayPause?()
    }
}
