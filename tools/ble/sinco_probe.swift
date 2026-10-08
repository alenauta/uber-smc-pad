// Read-only BLE probe for M-VAVE / Sinco devices (CoreBluetooth, run with `swift`).
//
// Connects to a device whose name contains FILTER (default "SMC"), lists all GATT
// services/characteristics, then sends ONLY the Sinco name/version query
// (cmd 0x11 on service AE40) and prints the decoded reply.
//
// Usage: swift tools/ble/sinco_probe.swift [FILTER] [TIMEOUT_SECONDS]
import CoreBluetooth
import Foundation

let args = CommandLine.arguments
let nameFilter = args.count > 1 ? args[1] : "SMC"
let timeout = args.count > 2 ? Double(args[2]) ?? 30 : 30

let sincoService = CBUUID(string: "AE40")
let sincoWrite = CBUUID(string: "AE41")
let sincoNotify = CBUUID(string: "AE42")
let bleMidiService = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
let jlRcspService = CBUUID(string: "AE00")

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined(separator: " ") }

/// Sinco packet: 00 59 | cmd | len24 LE | payload | ~sum(payload)
func sincoPacket(cmd: UInt8, payload: [UInt8] = []) -> Data {
    var p: [UInt8] = [0x00, 0x59, cmd,
                      UInt8(payload.count & 0xFF), UInt8((payload.count >> 8) & 0xFF), UInt8((payload.count >> 16) & 0xFF)]
    p += payload
    let sum = payload.reduce(0) { ($0 + Int($1)) & 0xFF }
    p.append(UInt8(~UInt8(sum)))
    return Data(p)
}

final class Probe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var target: CBPeripheral?
    var rx = Data()
    var done = false
    var pendingCharDiscovery = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func finish(_ msg: String) {
        print(msg)
        if let t = target { central.cancelPeripheralConnection(t) }
        done = true
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            // Already connected to macOS (e.g. via Audio MIDI Setup)?
            let connected = c.retrieveConnectedPeripherals(withServices: [sincoService, bleMidiService, jlRcspService])
            if let p = connected.first(where: { ($0.name ?? "").localizedCaseInsensitiveContains(nameFilter) }) {
                print("found already-connected peripheral: \(p.name ?? "?") \(p.identifier)")
                connect(p)
            } else {
                print("scanning for devices matching \"\(nameFilter)\" …")
                c.scanForPeripherals(withServices: nil, options: nil)
            }
        case .unauthorized: finish("ERROR: Bluetooth permission denied for this process")
        case .poweredOff: finish("ERROR: Bluetooth is off")
        case .unsupported: finish("ERROR: Bluetooth LE unsupported")
        default: break
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        let name = (ad[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard name.localizedCaseInsensitiveContains(nameFilter) else { return }
        print("discovered: \(name)  rssi=\(rssi)  id=\(p.identifier)")
        if let su = ad[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] { print("  adv services: \(su)") }
        c.stopScan()
        connect(p)
    }

    func connect(_ p: CBPeripheral) {
        target = p
        p.delegate = self
        central.connect(p, options: nil)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        print("connected; discovering services …")
        p.discoverServices(nil)
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        finish("ERROR: connect failed: \(error?.localizedDescription ?? "?")")
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = p.services, !services.isEmpty else { return finish("ERROR: no services") }
        pendingCharDiscovery = services.count
        for s in services { p.discoverCharacteristics(nil, for: s) }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        print("service \(s.uuid)")
        for ch in s.characteristics ?? [] {
            print("  char \(ch.uuid)  props=0x\(String(ch.properties.rawValue, radix: 16))")
        }
        pendingCharDiscovery -= 1
        guard pendingCharDiscovery == 0 else { return }

        guard let svc = p.services?.first(where: { $0.uuid == sincoService }),
              let notify = svc.characteristics?.first(where: { $0.uuid == sincoNotify }) else {
            return finish("Sinco service AE40/AE42 not found – nothing sent.")
        }
        p.setNotifyValue(true, for: notify)
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == sincoNotify else { return }
        if let e = error { return finish("ERROR enabling notify: \(e.localizedDescription)") }
        guard let w = p.services?.first(where: { $0.uuid == sincoService })?
                .characteristics?.first(where: { $0.uuid == sincoWrite }) else {
            return finish("AE41 write characteristic not found – nothing sent.")
        }
        let pkt = sincoPacket(cmd: 0x11)
        let type: CBCharacteristicWriteType = w.properties.contains(.write) ? .withResponse : .withoutResponse
        print("TX \(hex(pkt))  (CMD_QUERY_NAME_VER)")
        p.writeValue(pkt, for: w, type: type)
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == sincoNotify, let v = ch.value else { return }
        print("RX \(hex(v))")
        rx.append(v)
        if rx.count >= 7 {
            let b = [UInt8](rx)
            let len = Int(b[3]) | Int(b[4]) << 8 | Int(b[5]) << 16
            if b.count >= 6 + len + 1 {
                let payload = Array(b[6..<(6 + len)])
                let chk = b[6 + len]
                let calc = UInt8(~UInt8(payload.reduce(0) { ($0 + Int($1)) & 0xFF }))
                let ascii = String(decoding: payload.filter { $0 >= 0x20 && $0 < 0x7F }, as: UTF8.self)
                finish("""
                reply: head=\(String(format: "%02x %02x", b[0], b[1])) cmd=0x\(String(b[2], radix: 16)) len=\(len) \
                checksum \(chk == calc ? "OK" : "BAD (got \(chk), want \(calc))")
                payload ascii: "\(ascii)"
                """)
            }
        }
    }
}

let probe = Probe()
let deadline = Date().addingTimeInterval(timeout)
while !probe.done && Date() < deadline {
    RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.2))
}
if !probe.done { print("timeout after \(Int(timeout)) s (rx so far: \(hex(probe.rx)))") }
if let t = probe.target { probe.central.cancelPeripheralConnection(t) }
RunLoop.main.run(until: Date().addingTimeInterval(0.5))
