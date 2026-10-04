import UIKit

/// One of the player's sliders: a name, the current value, and a step either way.
///
/// To VoiceOver it is a single adjustable control: swipe up or down on it to step,
/// and the new value is read out. On screen it is the name and value between a
/// minus and a plus button.
final class AdjustableRow: UIView {
    /// Called with -1 or 1.
    var onStep: ((Int) -> Void)?

    var value: String = "" {
        didSet {
            valueLabel.text = value
            accessibilityValue = value
        }
    }

    private let nameLabel = UILabel()
    private let valueLabel = UILabel()
    private let minusButton = UIButton(type: .system)
    private let plusButton = UIButton(type: .system)

    init(name: String, hint: String, minusSymbol: String = "minus.circle", plusSymbol: String = "plus.circle") {
        super.init(frame: .zero)

        nameLabel.text = name
        nameLabel.font = .preferredFont(forTextStyle: .caption1)
        nameLabel.textColor = .secondaryLabel
        nameLabel.adjustsFontForContentSizeCategory = true
        nameLabel.textAlignment = .center

        valueLabel.font = .preferredFont(forTextStyle: .title3)
        valueLabel.adjustsFontForContentSizeCategory = true
        valueLabel.textAlignment = .center
        valueLabel.numberOfLines = 2
        valueLabel.adjustsFontSizeToFitWidth = true
        valueLabel.minimumScaleFactor = 0.6

        configure(minusButton, symbol: minusSymbol, step: -1)
        configure(plusButton, symbol: plusSymbol, step: 1)

        let text = UIStackView(arrangedSubviews: [nameLabel, valueLabel])
        text.axis = .vertical
        text.spacing = 2
        let row = UIStackView(arrangedSubviews: [minusButton, text, plusButton])
        row.axis = .horizontal
        row.alignment = .center
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        NSLayoutConstraint.activate([
            row.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            row.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(equalTo: trailingAnchor),
            minusButton.widthAnchor.constraint(equalToConstant: 56),
            plusButton.widthAnchor.constraint(equalToConstant: 56),
            minusButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 48),
            plusButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 48),
        ])

        isAccessibilityElement = true
        accessibilityTraits = .adjustable
        accessibilityLabel = name
        accessibilityHint = hint
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private func configure(_ button: UIButton, symbol: String, step: Int) {
        let size = UIImage.SymbolConfiguration(textStyle: .title1)
        button.setImage(UIImage(systemName: symbol, withConfiguration: size), for: .normal)
        button.addAction(UIAction { [weak self] _ in self?.onStep?(step) }, for: .touchUpInside)
    }

    override func accessibilityIncrement() { onStep?(1) }
    override func accessibilityDecrement() { onStep?(-1) }
}
