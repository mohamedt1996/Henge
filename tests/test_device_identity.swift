import Foundation

@main
struct DeviceIdentityTests {
    static func main() {
        let sequoia = "macOS Sequoia 15.6.1"
        let tahoe = "macOS Tahoe 26.6.2"
        let mac27 = "macOS 27.2"

        // What each guest macOS recognises.
        precondition(DeviceIdentity.knowniPhoneGeneration(forOS: sequoia) == 16)
        precondition(DeviceIdentity.knowniPhoneGeneration(forOS: tahoe) == 17)
        precondition(DeviceIdentity.knowniPhoneGeneration(forOS: mac27) == 18)
        precondition(DeviceIdentity.knowniPhoneGeneration(forOS: nil) == 16)

        // Newer than the guest: snap to the newest equivalent, same variant.
        // An iPhone 18 Pro Max on Sequoia is a 16 Pro Max as far as the icon goes.
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,7", forOS: sequoia) == "iPhone17,2")
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,3", forOS: sequoia) == "iPhone17,2")
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,2", forOS: sequoia) == "iPhone17,1")
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,7", forOS: tahoe) == "iPhone18,2")
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,2", forOS: tahoe) == "iPhone18,1")
        precondition(DeviceIdentity.snappedIdentifier("iPhone18,5", forOS: sequoia) == "iPhone17,5") // 17e -> 16e
        precondition(DeviceIdentity.snappedIdentifier("iPhone18,3", forOS: sequoia) == "iPhone17,3") // 17 -> 16

        // The guest knows the generation: the exact identifier, variant and all.
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,7", forOS: mac27) == "iPhone19,7")
        precondition(DeviceIdentity.snappedIdentifier("iPhone17,2", forOS: sequoia) == "iPhone17,2")
        precondition(DeviceIdentity.snappedIdentifier("iPhone13,4", forOS: mac27) == "iPhone13,4")
        // Older than the guest (the XR example): exact, even outside the table.
        precondition(DeviceIdentity.snappedIdentifier("iPhone11,8", forOS: mac27) == "iPhone11,8")

        // Special iPhones and iPads have no generation to snap: exact.
        precondition(DeviceIdentity.snappedIdentifier("iPhone18,4", forOS: sequoia) == "iPhone18,4") // Air
        precondition(DeviceIdentity.snappedIdentifier("iPhone19,4", forOS: sequoia) == "iPhone19,4") // Duo
        precondition(DeviceIdentity.snappedIdentifier("iPhone14,6", forOS: sequoia) == "iPhone14,6") // SE
        precondition(DeviceIdentity.snappedIdentifier("iPad16,3", forOS: sequoia) == "iPad16,3")
        precondition(DeviceIdentity.snappedIdentifier("iPhone20,1", forOS: sequoia) == "iPhone20,1") // unknown: exact

        // The reverse lookup picks the canonical (lowest) identifier of a generation.
        precondition(DeviceIdentity.identifier(generation: 16, variant: "Pro Max") == "iPhone17,2")
        precondition(DeviceIdentity.identifier(generation: 18, variant: "Pro Max") == "iPhone19,3")
        precondition(DeviceIdentity.identifier(generation: 16, variant: "base") == "iPhone17,3")

        // Names and chips: the display name is always the real one.
        precondition(DeviceIdentity.name("iPhone19,7") == "iPhone 18 Pro Max")
        precondition(DeviceIdentity.chip("iPhone19,7") == "Apple A20 Pro")
        precondition(DeviceIdentity.name("iPhone17,2") == "iPhone 16 Pro Max")
        precondition(DeviceIdentity.name("iPhone20,1") == "iPhone20,1") // unknown: the identifier
        precondition(DeviceIdentity.chip("iPhone20,1") == nil)
        // Every name fits the firmware's 23-character product-name slot.
        for (_, d) in DeviceIdentity.table {
            precondition(d.name.count <= 23, "\(d.name) is too long for the product-name slot")
        }
        print("Device identity: all snapping checks passed")
    }
}