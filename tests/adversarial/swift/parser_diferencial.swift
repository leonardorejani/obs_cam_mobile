import Foundation
var rng = SystemRandomNumberGenerator(); var d1 = 0, d2 = 0, errs = 0, perdeu = 0
for _ in 0..<20000 {
    var b = Data()
    for _ in 0..<(1 + Int.random(in: 0..<5, using: &rng)) {
        if Int.random(in: 0..<6, using: &rng) == 0 { b.append(Data((0..<Int.random(in: 0..<30, using: &rng)).map { _ in UInt8.random(in: 0...255, using: &rng) })) }
        else { let tipo = [1,2,3,4,9,10,77].randomElement(using: &rng)!; b.append(Pacote(tipo: UInt8(tipo), tempo: UInt64.random(in: 0...UInt64.max, using: &rng), dados: Data((0..<Int.random(in: 0..<200, using: &rng)).map { _ in UInt8.random(in: 0...255, using: &rng) })).serializar()) }
    }
    var p1 = ParserPacotes(), p2 = ParserPacotes(), p3 = ParserPacotes()
    var e1 = false, e2 = false, e3 = false; var r1: [Pacote] = [], r2: [Pacote] = [], r3: [Pacote] = []
    do { r1 = try p1.alimentar(b) } catch { e1 = true }
    for i in 0..<b.count { if e2 { break }; do { r2 += try p2.alimentar(b.subdata(in: i..<i+1)) } catch { e2 = true } }
    var off = 0; while off < b.count && !e3 { let n = min(Int.random(in: 1...17, using: &rng), b.count - off); do { r3 += try p3.alimentar(b.subdata(in: off..<off+n)) } catch { e3 = true }; off += n }
    if e1 { errs += 1 }
    if e1 != e2 || e2 != e3 { d1 += 1 }
    if !e1 && !e2 && (r1 != r2 || r2 != r3) { d2 += 1 }
    if e1 && !r2.isEmpty && r1.isEmpty { perdeu += 1 }
}
print("erro diverge: \(d1), pacotes divergem sem erro: \(d2), fluxos com erro: \(errs), pacotes validos perdidos pelo throw no mesmo chunk: \(perdeu)")
