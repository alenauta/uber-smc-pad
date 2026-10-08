// Read-only Sinco memory reader over BLE (CoreBluetooth, run with `swift`).
//
// SAFETY: this tool can only emit CMD_QUERY_NAME_VER (0x11) and CMD_READ (0x23).
// There is deliberately no code path that builds a CMD_WRITE (0x22) packet.
//
// Usage:
//   swift tools/ble/sinco_read.swift [--name SMC] [--max 64] READ [READ ...]
//   READ = type:address:length   (decimal or 0x-hex), e.g. 0:0x0:16  5:0x80000000:4
//   Ranges for scanning: type 0-7 → use the shell to expand, or "0-7:0:16".
//
// Output: one line per read:  OK|ERR|TIMEOUT  type addr len  <hex>  |ascii|
//
// Dump mode (raw SPI flash via type 0, read-only):
//   swift tools/ble/sinco_read.swift --dump out.bin 0x0 0x80000 [--chunk 512]
//   Writes the bytes to out.bin; aborts on any error/timeout (no partial-file surprises).
import CoreBluetooth
import Foundation

// ---------- argument parsing ----------
struct ReadReq { let type: UInt8; let addr: UInt32; let len: Int }

func num(_ s: String) -> UInt64? {
    s.lowercased().hasPrefix("0x") ? UInt64(s.dropFirst(2), radix: 16) : UInt64(s)
}

var nameFilter = "SMC"
var maxLen = 64
var reqs: [ReadReq] = []
var dumpPath: String? = nil
var chunk = 512
var it = CommandLine.arguments.dropFirst().makeIterator()
while let a = it.next() {
    switch a {
    case "--name": nameFilter = it.next() ?? nameFilter
    case "--max": maxLen = Int(it.next() ?? "") ?? maxLen
    case "--chunk": chunk = Int(it.next() ?? "") ?? chunk
    case "--dump":
        dumpPath = it.next()
        guard let s = it.next().flatMap(num), let e = it.next().flatMap(num), e > s else {
            fputs("usage: --dump FILE START END\n", stderr); exit(2)
        }
        var addr = s
        while addr < e {
            let n = Int(min(UInt64(chunk), e - addr))
            reqs.append(ReadReq(type: 0, addr: UInt32(addr), len: n)); addr += UInt64(n)
        }
    default:
        let parts = a.split(separator: ":").map(String.init)
        guard parts.count == 3, let addr = num(parts[1]), let len = num(parts[2]) else {
            fputs("bad read spec: \(a)\n", stderr); exit(2)
        }
        let typeRange: ClosedRange<UInt64>
        if parts[0].contains("-") {
            let tr = parts[0].split(separator: "-").compactMap { num(String($0)) }
            typeRange = tr[0]...tr[1]
        } else { typeRange = num(parts[0])!...num(parts[0])! }
        guard Int(len) <= maxLen, len > 0 else { fputs("length \(len) exceeds --max \(maxLen)\n", stderr); exit(2) }
        for t in typeRange { reqs.append(ReadReq(type: UInt8(t), addr: UInt32(addr), len: Int(len))) }
    }
}
if reqs.isEmpty { fputs("no reads given\n", stderr); exit(2) }
// Type 0 = raw SPI flash (512 KB). Out-of-range addresses are unsafe: a read at
// 0xFFFFE0 made the pad drop the BLE link (likely a fault + reset). Refuse them.
let flashSize: UInt64 = 0x80000
if let bad = reqs.first(where: { $0.type == 0 && UInt64($0.addr) + UInt64($0.len) > flashSize }) {
    fputs(String(format: "refusing type-0 read beyond flash end: addr=0x%08x len=%d\n", bad.addr, bad.len), stderr); exit(2)
}

// ---------- protocol ----------
let sincoService = CBUUID(string: "AE40"), sincoWrite = CBUUID(string: "AE41"), sincoNotify = CBUUID(string: "AE42")
let bleMidi = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")

func le(_ v: UInt64, _ n: Int) -> [UInt8] { (0..<n).map { UInt8((v >> (8 * UInt64($0))) & 0xFF) } }
func chk(_ p: [UInt8]) -> UInt8 { ~UInt8(p.reduce(0) { ($0 + Int($1)) & 0xFF }) }
func packet(_ cmd: UInt8, _ payload: [UInt8]) -> Data {
    precondition(cmd == 0x11 || cmd == 0x23, "only query/read commands are allowed")
    return Data([0x00, 0x59, cmd] + le(UInt64(payload.count), 3) + payload + [chk(payload)])
}
func readPacket(_ r: ReadReq) -> Data { packet(0x23, [r.type] + le(UInt64(r.addr), 4) + le(UInt64(r.len), 3)) }
func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined(separator: " ") }
func ascii(_ b: [UInt8]) -> String { String(b.map { (0x20..<0x7F).contains($0) ? Character(UnicodeScalar($0)) : "." }) }

// ---------- BLE session ----------
final class Session: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var periph: CBPeripheral?
    var wChar: CBCharacteristic?
    var rx: [UInt8] = []
    var idx = -1               // -1 = name query, then reqs[0...]
    var sentAt = Date()
    var finished = false
    var aborted = false

    override init() { super.init(); central = CBCentralManager(delegate: self, queue: nil) }

    func stop(_ msg: String? = nil, abort: Bool = false) {
        if let m = msg { print(m) }
        aborted = aborted || abort
        finished = true
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else {
            if c.state == .unauthorized || c.state == .poweredOff { stop("ERROR: Bluetooth state \(c.state.rawValue)", abort: true) }
            return
        }
        if let p = c.retrieveConnectedPeripherals(withServices: [sincoService, bleMidi])
            .first(where: { ($0.name ?? "").localizedCaseInsensitiveContains(nameFilter) }) {
            connect(p)
        } else { c.scanForPeripherals(withServices: nil) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        let n = (ad[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard n.localizedCaseInsensitiveContains(nameFilter) else { return }
        c.stopScan(); connect(p)
    }
    func connect(_ p: CBPeripheral) { periph = p; p.delegate = self; central.connect(p) }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) { p.discoverServices([sincoService]) }
    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        if !finished { stop("ABORT: device disconnected (\(error?.localizedDescription ?? "-"))", abort: true) }
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == sincoService }) else { return stop("AE40 not found", abort: true) }
        p.discoverCharacteristics([sincoWrite, sincoNotify], for: s)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        wChar = s.characteristics?.first { $0.uuid == sincoWrite }
        guard let n = s.characteristics?.first(where: { $0.uuid == sincoNotify }), wChar != nil else { return stop("AE41/AE42 missing", abort: true) }
        p.setNotifyValue(true, for: n)
    }
    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == sincoNotify, error == nil else { return stop("notify failed", abort: true) }
        send(packet(0x11, []))
    }

    func send(_ d: Data) {
        rx = []; sentAt = Date()
        periph?.writeValue(d, for: wChar!, type: .withoutResponse)
    }

    func next() {
        idx += 1
        if idx >= reqs.count { return stop("done: \(reqs.count) reads") }
        send(readPacket(reqs[idx]))
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == sincoNotify, let v = ch.value else { return }
        rx += [UInt8](v)
        guard rx.count >= 7, rx[0] == 0x00, rx[1] == 0x59 else {
            if rx.count >= 2 && !(rx[0] == 0x00 && rx[1] == 0x59) { return stop("ABORT: unexpected bytes \(hex(rx))", abort: true) }
            return
        }
        let len = Int(rx[3]) | Int(rx[4]) << 8 | Int(rx[5]) << 16
        guard rx.count >= 6 + len + 1 else { return }   // wait for more chunks
        let payload = Array(rx[6..<(6 + len)])
        let ok = rx[6 + len] == chk(payload)
        if idx == -1 {
            print("device: \(ascii(payload).trimmingCharacters(in: CharacterSet(charactersIn: ".")))  (checksum \(ok ? "OK" : "BAD"))")
            return next()
        }
        let r = reqs[idx]
        let tag = String(format: "t=%u addr=0x%08x len=%-3d", r.type, r.addr, r.len)
        guard ok else { return stop("ABORT: checksum mismatch \(tag): \(hex(rx))", abort: true) }
        guard rx[2] == 0x23, payload.count >= 8 else {
            print("ERR  \(tag)  cmd=0x\(String(rx[2], radix: 16)) raw=\(hex(rx))"); return next()
        }
        let data = Array(payload[8...])
        let echo = payload[0] == r.type && Array(payload[1...4]) == le(UInt64(r.addr), 4)
        if dumpPath != nil {
            guard echo, data.count == r.len else { return stop("ABORT: bad reply for \(tag): \(hex(Array(payload.prefix(16))))", abort: true) }
            dump += data
            if idx % 64 == 0 || idx == reqs.count - 1 {
                print(String(format: "  %6.1f%%  0x%06x", Double(idx + 1) * 100 / Double(reqs.count), r.addr + UInt32(r.len)))
            }
            return next()
        }
        print("OK   \(tag)  got=\(data.count)\(echo ? "" : " (hdr≠req: \(hex(Array(payload[0..<8]))))")  \(hex(data))  |\(ascii(data))|")
        next()
    }
    var dump: [UInt8] = []

    func tick() {
        guard !finished, idx >= -1, Date().timeIntervalSince(sentAt) > 3, periph?.state == .connected, wChar != nil else { return }
        let what = idx == -1 ? "name query" : String(format: "t=%u addr=0x%08x", reqs[idx].type, reqs[idx].addr)
        print("TIMEOUT \(what)  partial=\(hex(rx))")
        if dumpPath != nil { return stop("ABORT: timeout during dump", abort: true) }
        // Two silent reads in a row → stop to be safe.
        if idx >= 0, rx.isEmpty, lastTimeout == idx - 1 { return stop("ABORT: two consecutive timeouts", abort: true) }
        lastTimeout = idx
        if idx == -1 { return stop("ABORT: no reply to name query", abort: true) }
        next()
    }
    var lastTimeout = -99
}

let s = Session()
let hardDeadline = Date().addingTimeInterval(Double(30 + reqs.count * 4))
while !s.finished && Date() < hardDeadline {
    RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.1))
    s.tick()
}
if !s.finished { print("ABORT: overall timeout") }
if let p = s.periph { s.central.cancelPeripheralConnection(p) }
RunLoop.main.run(until: Date().addingTimeInterval(0.5))
if let path = dumpPath {
    if s.aborted || s.dump.count != reqs.reduce(0, { $0 + $1.len }) {
        print("dump NOT written (aborted or incomplete: \(s.dump.count) bytes)"); exit(1)
    }
    FileManager.default.createFile(atPath: path, contents: Data(s.dump))
    print("wrote \(s.dump.count) bytes to \(path)")
}
exit(s.aborted ? 1 : 0)
