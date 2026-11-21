import Foundation

/// The host device's identity, and how to report it to a guest macOS so the guest
/// shows a matching icon. Pure Foundation (no UIKit) so the Mac tests can compile it.
///
/// Identifiers and names: adamawolf/3048717 (definitive) and AppleDB. Names fit the
/// firmware's 23-character product-name slot, so generation-numbered iPads use their
/// chip ("iPad Pro 11-inch (M1)"). Anything newer than the table reports its identifier.
enum DeviceIdentity {
    struct Device {
        let name: String
        let chip: String
        let generation: Int?   /// iPhone marketing generation (12...18); nil for iPad/special
        let variant: String?   /// mini, base, Plus, Pro, Pro Max, e; nil for iPad/special
        init(_ name: String, _ chip: String, _ generation: Int? = nil, _ variant: String? = nil) {
            self.name = name
            self.chip = chip
            self.generation = generation
            self.variant = variant
        }
    }

    static func device(_ id: String) -> Device? { table[id] }
    static func name(_ id: String) -> String { table[id]?.name ?? id }
    static func chip(_ id: String) -> String? { table[id]?.chip }

    /// The model identifier to report so the guest shows a matching icon: the real one
    /// when the guest macOS knows it, otherwise the newest equivalent it does recognise
    /// (same variant). An iPhone 18 Pro Max on Sequoia reports iPhone17,2 (16 Pro Max,
    /// the newest Pro Max Sequoia knows) while the display name stays "iPhone 18 Pro Max".
    /// iPads and special iPhones (Air, SE, Duo) report their real identifier.
    static func snappedIdentifier(_ id: String, forOS osName: String?) -> String {
        guard id.hasPrefix("iPhone"), let d = table[id],
              let gen = d.generation, let variant = d.variant else { return id }
        let cap = knowniPhoneGeneration(forOS: osName)
        if gen <= cap { return id } /// the guest knows this generation: exact
        return identifier(generation: cap, variant: variant)
            ?? identifier(generation: cap, variant: "base") /// variant discontinued by then
            ?? id
    }

    /// The newest iPhone generation a macOS release recognises in CoreTypes.bundle:
    /// Sequoia 15 -> iPhone 16, macOS 26 -> iPhone 17, macOS 27 -> iPhone 18.
    static func knowniPhoneGeneration(forOS osName: String?) -> Int {
        guard let os = osName,
              let r = os.range(of: #"\d+\.\d+"#, options: .regularExpression),
              let major = Int(os[r].prefix(while: { $0.isNumber })) else { return 16 }
        return major >= 26 ? major - 9 : major + 1
    }

    /// The canonical (lowest) identifier for an iPhone generation and variant.
    static func identifier(generation: Int, variant: String) -> String? {
        var best: String?
        for (id, d) in table where d.generation == generation && d.variant == variant {
            if let b = best { if id.compare(b, options: .numeric) == .orderedAscending { best = id } }
            else { best = id }
        }
        return best
    }

    static let table: [String: Device] = [
        "iPhone13,1": Device("iPhone 12 mini", "Apple A14", 12, "mini"),
        "iPhone13,2": Device("iPhone 12", "Apple A14", 12, "base"),
        "iPhone13,3": Device("iPhone 12 Pro", "Apple A14", 12, "Pro"),
        "iPhone13,4": Device("iPhone 12 Pro Max", "Apple A14", 12, "Pro Max"),
        "iPhone14,2": Device("iPhone 13 Pro", "Apple A15", 13, "Pro"),
        "iPhone14,3": Device("iPhone 13 Pro Max", "Apple A15", 13, "Pro Max"),
        "iPhone14,4": Device("iPhone 13 mini", "Apple A15", 13, "mini"),
        "iPhone14,5": Device("iPhone 13", "Apple A15", 13, "base"),
        "iPhone14,6": Device("iPhone SE (A15)", "Apple A15"),
        "iPhone14,7": Device("iPhone 14", "Apple A15", 14, "base"),
        "iPhone14,8": Device("iPhone 14 Plus", "Apple A15", 14, "Plus"),
        "iPhone15,2": Device("iPhone 14 Pro", "Apple A16", 14, "Pro"),
        "iPhone15,3": Device("iPhone 14 Pro Max", "Apple A16", 14, "Pro Max"),
        "iPhone15,4": Device("iPhone 15", "Apple A16", 15, "base"),
        "iPhone15,5": Device("iPhone 15 Plus", "Apple A16", 15, "Plus"),
        "iPhone16,1": Device("iPhone 15 Pro", "Apple A17 Pro", 15, "Pro"),
        "iPhone16,2": Device("iPhone 15 Pro Max", "Apple A17 Pro", 15, "Pro Max"),
        "iPhone17,1": Device("iPhone 16 Pro", "Apple A18 Pro", 16, "Pro"),
        "iPhone17,2": Device("iPhone 16 Pro Max", "Apple A18 Pro", 16, "Pro Max"),
        "iPhone17,3": Device("iPhone 16", "Apple A18", 16, "base"),
        "iPhone17,4": Device("iPhone 16 Plus", "Apple A18", 16, "Plus"),
        "iPhone17,5": Device("iPhone 16e", "Apple A18", 16, "e"),
        "iPhone18,1": Device("iPhone 17 Pro", "Apple A19 Pro", 17, "Pro"),
        "iPhone18,2": Device("iPhone 17 Pro Max", "Apple A19 Pro", 17, "Pro Max"),
        "iPhone18,3": Device("iPhone 17", "Apple A19", 17, "base"),
        "iPhone18,4": Device("iPhone Air", "Apple A19 Pro"),
        "iPhone18,5": Device("iPhone 17e", "Apple A19", 17, "e"),
        "iPhone19,2": Device("iPhone 18 Pro", "Apple A20 Pro", 18, "Pro"),
        "iPhone19,3": Device("iPhone 18 Pro Max", "Apple A20 Pro", 18, "Pro Max"),
        "iPhone19,7": Device("iPhone 18 Pro Max", "Apple A20 Pro", 18, "Pro Max"),
        "iPhone19,4": Device("iPhone Duo", "Apple A20 Pro"),
        "iPad13,1": Device("iPad Air (A14)", "Apple A14"),
        "iPad13,2": Device("iPad Air (A14)", "Apple A14"),
        "iPad13,4": Device("iPad Pro 11-inch (M1)", "Apple M1"),
        "iPad13,5": Device("iPad Pro 11-inch (M1)", "Apple M1"),
        "iPad13,6": Device("iPad Pro 11-inch (M1)", "Apple M1"),
        "iPad13,7": Device("iPad Pro 11-inch (M1)", "Apple M1"),
        "iPad13,8": Device("iPad Pro 12.9-inch (M1)", "Apple M1"),
        "iPad13,9": Device("iPad Pro 12.9-inch (M1)", "Apple M1"),
        "iPad13,10": Device("iPad Pro 12.9-inch (M1)", "Apple M1"),
        "iPad13,11": Device("iPad Pro 12.9-inch (M1)", "Apple M1"),
        "iPad13,16": Device("iPad Air (M1)", "Apple M1"),
        "iPad13,17": Device("iPad Air (M1)", "Apple M1"),
        "iPad13,18": Device("iPad (10th generation)", "Apple A14"),
        "iPad13,19": Device("iPad (10th generation)", "Apple A14"),
        "iPad14,1": Device("iPad mini (A15)", "Apple A15"),
        "iPad14,2": Device("iPad mini (A15)", "Apple A15"),
        "iPad14,3": Device("iPad Pro 11-inch (M2)", "Apple M2"),
        "iPad14,4": Device("iPad Pro 11-inch (M2)", "Apple M2"),
        "iPad14,5": Device("iPad Pro 12.9-inch (M2)", "Apple M2"),
        "iPad14,6": Device("iPad Pro 12.9-inch (M2)", "Apple M2"),
        "iPad14,8": Device("iPad Air 11-inch (M2)", "Apple M2"),
        "iPad14,9": Device("iPad Air 11-inch (M2)", "Apple M2"),
        "iPad14,10": Device("iPad Air 13-inch (M2)", "Apple M2"),
        "iPad14,11": Device("iPad Air 13-inch (M2)", "Apple M2"),
        "iPad15,3": Device("iPad Air 11-inch (M3)", "Apple M3"),
        "iPad15,4": Device("iPad Air 11-inch (M3)", "Apple M3"),
        "iPad15,5": Device("iPad Air 13-inch (M3)", "Apple M3"),
        "iPad15,6": Device("iPad Air 13-inch (M3)", "Apple M3"),
        "iPad15,7": Device("iPad (A16)", "Apple A16"),
        "iPad15,8": Device("iPad (A16)", "Apple A16"),
        "iPad16,1": Device("iPad mini (A17 Pro)", "Apple A17 Pro"),
        "iPad16,2": Device("iPad mini (A17 Pro)", "Apple A17 Pro"),
        "iPad16,3": Device("iPad Pro 11-inch (M4)", "Apple M4"),
        "iPad16,4": Device("iPad Pro 11-inch (M4)", "Apple M4"),
        "iPad16,5": Device("iPad Pro 13-inch (M4)", "Apple M4"),
        "iPad16,6": Device("iPad Pro 13-inch (M4)", "Apple M4"),
    ]
}