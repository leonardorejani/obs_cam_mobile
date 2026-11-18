import Foundation
import Darwin

func rssMB() -> Double {
    var info = mach_task_basic_info()
    var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
    let kr = withUnsafeMutablePointer(to: &info) { $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count) } }
    return kr == KERN_SUCCESS ? Double(info.resident_size) / 1_048_576 : -1
}
func say(_ s: String) { print(s); fflush(stdout) }

// ---------- cliente cru ----------
final class Cli {
    var fd: Int32
    init?(porta: UInt16) {
        fd = socket(AF_INET, SOCK_STREAM, 0)
        var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
        a.sin_port = porta.bigEndian; a.sin_addr.s_addr = inet_addr("127.0.0.1")
        let r = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        if r != 0 { close(fd); return nil }
        var one: Int32 = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, 4)
    }
    @discardableResult func enviar(_ d: Data) -> Bool { d.withUnsafeBytes { p in var off = 0; while off < d.count { let n = send(fd, p.baseAddress! + off, d.count - off, 0); if n <= 0 { return false }; off += n }; return true } }
    @discardableResult func enviar(_ s: String) -> Bool { enviar(Data(s.utf8)) }
    /// le ate `seg`s ou EOF/erro. Devolve (bytes, motivo)
    func ler(ate seg: Double, parar: ((Data) -> Bool)? = nil) -> (Data, String) {
        var out = Data(); let fim = Date().addingTimeInterval(seg)
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let resto = fim.timeIntervalSinceNow; if resto <= 0 { return (out, "timeout") }
            var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let pr = poll(&p, 1, Int32(min(resto * 1000, 200)))
            if pr == 0 { continue }
            let n = recv(fd, &buf, buf.count, 0)
            if n == 0 { return (out, "EOF") }
            if n < 0 { return (out, "errno \(errno) \(String(cString: strerror(errno)))") }
            out.append(buf, count: n)
            if let f = parar, f(out) { return (out, "ok") }
        }
    }
    func linha(_ seg: Double = 5) -> (String?, String) {
        let (d, m) = ler(ate: seg) { $0.contains(0x0A) }
        if let i = d.firstIndex(of: 0x0A) { return (String(decoding: d[d.startIndex..<i], as: UTF8.self), m) }
        return (nil, m + " bytes=\(d.count)")
    }
    func fechar() { close(fd) }
}

// ---------- fontes falsas ----------
final class Fonte: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    var atrasoInicio: Double = 0
    var falha: ErroFonte?
    var chaveBytes = 20_000, deltaBytes = 3_000
    var periodo = 1.0 / 30, chaveACada = 60
    var emitir = true
    let t = NSLock()
    var _parou = 0, _kf = 0, _emitidos = 0, _iniciou = 0
    var parouN: Int { t.lock(); defer { t.unlock() }; return _parou }
    var kfN: Int { t.lock(); defer { t.unlock() }; return _kf }
    var emitidos: Int { t.lock(); defer { t.unlock() }; return _emitidos }
    var timer: DispatchSourceTimer?
    var ativo = false
    func iniciar(_ c: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        t.lock(); _iniciou += 1; t.unlock()
        DispatchQueue.global().asyncAfter(deadline: .now() + atrasoInicio) { [self] in
            if let f = falha { completion(.failure(f)); return }
            completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
            if !emitir { return }
            ativo = true
            aoEvento?(.config(tempo: 1000, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
            var i = 0
            let tm = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "fonte"))
            tm.schedule(deadline: .now(), repeating: periodo)
            tm.setEventHandler { [self] in
                let chave = i % chaveACada == 0
                t.lock(); _emitidos += 1; t.unlock()
                aoEvento?(.quadro(tempo: UInt64(1000 + i * 33333), chave: chave, dados: Data(count: chave ? chaveBytes : deltaBytes)))
                i += 1
            }
            tm.resume(); timer = tm
        }
    }
    func parar() { t.lock(); _parou += 1; t.unlock(); timer?.cancel() }
    func pedirKeyframe() { t.lock(); _kf += 1; t.unlock() }
}

let ident = IdentidadeAparelho(modelo: "iPhone14,5", nome: "N", id: "ID")
func subir(_ criar: @escaping () -> Fonte, _ aj: (inout Servidor.Opcoes) -> Void = { _ in }) -> (Servidor, UInt16) {
    var op = Servidor.Opcoes(); op.porta = 0; aj(&op)
    let s = Servidor(opcoes: op, identidade: ident, criarFonte: { criar() })
    s.iniciar()
    let fim = Date().addingTimeInterval(5)
    while s.portaAtual == nil && Date() < fim { usleep(10_000) }
    return (s, s.portaAtual!)
}
let hs = #"{"v":1,"cliente":"adv"}"# + "\n"

// ======== A. Handshake diferencial ========
say("=== A. Handshake.interpretar ===")
let casos: [(String, Data)] = [
    ("v=1", Data(#"{"v":1}"#.utf8)),
    ("v=2000000", Data(#"{"v":2000000}"#.utf8)),
    ("v=-1", Data(#"{"v":-1}"#.utf8)),
    ("v=1e15", Data(#"{"v":1e15}"#.utf8)),
    ("v=1e400", Data(#"{"v":1e400}"#.utf8)),
    ("v=true", Data(#"{"v":true}"#.utf8)),
    ("v=1.5", Data(#"{"v":1.5}"#.utf8)),
    ("v=1.0", Data(#"{"v":1.0}"#.utf8)),
    ("bitrate=50000000.0", Data(#"{"v":1,"bitrate":50000000.0}"#.utf8)),
    ("bitrate=1e400", Data(#"{"v":1,"bitrate":1e400}"#.utf8)),
    ("bitrate=\"x\"", Data(#"{"v":1,"bitrate":"10000000"}"#.utf8)),
    ("duplicate v 1,2", Data(#"{"v":1,"v":2}"#.utf8)),
    ("cliente utf8 invalido", Data([0x7B,0x22,0x76,0x22,0x3A,0x31,0x2C,0x22,0x63,0x22,0x3A,0x22,0xFF,0xFE,0x22,0x7D])),
    ("NUL cru em string", Data([0x7B,0x22,0x76,0x22,0x3A,0x31,0x2C,0x22,0x63,0x22,0x3A,0x22,0x00,0x22,0x7D])),
    ("lixo apos JSON", Data(#"{"v":1} x"#.utf8)),
    ("BOM", Data([0xEF,0xBB,0xBF]) + Data(#"{"v":1}"#.utf8)),
    ("CR final", Data("{\"v\":1}\r".utf8)),
    ("surrogate solto", Data(#"{"v":1,"cliente":"\ud800"}"#.utf8)),
    ("array de 2001 nums", Data(("{\"v\":1,\"b\":[" + String(repeating: "1,", count: 2000) + "1]}").utf8)),
    ("objeto aninhado 100000", Data(("{\"v\":1,\"x\":" + String(repeating: "{\"a\":", count: 100000) + "1" + String(repeating: "}", count: 100000) + "}").utf8)),
    ("array aninhado 100000", Data(("{\"v\":1,\"x\":" + String(repeating: "[", count: 100000) + String(repeating: "]", count: 100000) + "}").utf8)),
    ("raiz array", Data("[1]".utf8)), ("vazio", Data()),
]
for (n, d) in casos { say("  \(n.padding(toLength: 26, withPad: " ", startingAt: 0)) -> \(Handshake.interpretar(linha: d))") }
for n in [4094, 4095, 4096, 4097] {
    var s = #"{"v":1,"cliente":""#; s += String(repeating: "a", count: n - s.utf8.count - 2); s += #""}"#
    say("  linha de \(s.utf8.count) bytes -> \(Handshake.interpretar(linha: Data(s.utf8)))")
}

// ======== B. Parser ========
say("=== B. ParserPacotes ===")
func cab(_ tipo: UInt8, _ tam: UInt32) -> Data { var d = Data([tipo]); d.append(Pacote.u64(5)); for i in (0..<4).reversed() { d.append(UInt8((tam >> UInt32(8*i)) & 0xFF)) }; return d }
for tam in [UInt32(0), 8388607, 8388608, 8388609, 0xFFFFFFFF] {
    for tipo in [UInt8(2), 77] {
        var p = ParserPacotes()
        do { let r = try p.alimentar(cab(tipo, tam)); say("  tipo \(tipo) tam \(tam) -> pacotes=\(r.count) sem erro") } catch { say("  tipo \(tipo) tam \(tam) -> ERRO \(error)") }
    }
}
do {
    var p = ParserPacotes(); let t0 = Date()
    var b = cab(77, 8_388_608); b.append(Data(count: 8_388_608)); b.append(Pacote(tipo: 9, tempo: 42, dados: Data()).serializar())
    var total = 0; var off = 0
    while off < b.count { let n = min(1400, b.count - off); total += try p.alimentar(b.subdata(in: off..<off+n)).count; off += n }
    say("  8MiB desconhecido em fatias de 1400B + ping: pacotes=\(total) em \(String(format: "%.2f", Date().timeIntervalSince(t0)))s")
} catch { say("  8MiB desconhecido -> ERRO \(error)") }
do {
    // 8MiB valido (quadro) em fatias de 1400: custo
    var p = ParserPacotes(); let t0 = Date()
    var b = Pacote(tipo: 2, tempo: 1, dados: Data(count: 8_388_608)).serializar()
    var total = 0; var off = 0
    while off < b.count { let n = min(1400, b.count - off); total += try p.alimentar(b.subdata(in: off..<off+n)).count; off += n }
    b = Data()
    say("  quadro 8MiB em fatias de 1400B: pacotes=\(total) em \(String(format: "%.2f", Date().timeIntervalSince(t0)))s")
} catch { say("  quadro 8MiB -> ERRO \(error)") }
do {
    // fuzz diferencial: um byte por vez vs tudo
    var rng = SystemRandomNumberGenerator(); var diverg = 0
    for _ in 0..<5000 {
        var b = Data()
        for _ in 0..<(1 + Int.random(in: 0..<5, using: &rng)) {
            if Int.random(in: 0..<4, using: &rng) == 0 { b.append(Data((0..<Int.random(in: 0..<30, using: &rng)).map { _ in UInt8.random(in: 0...255, using: &rng) })) }
            else { let tipo = [1,2,3,4,9,10,77].randomElement(using: &rng)! ; b.append(Pacote(tipo: UInt8(tipo), tempo: UInt64.random(in: 0...UInt64.max, using: &rng), dados: Data((0..<Int.random(in: 0..<200, using: &rng)).map { _ in UInt8.random(in: 0...255, using: &rng) })).serializar()) }
        }
        var p1 = ParserPacotes(), p2 = ParserPacotes()
        var e1 = false, e2 = false; var r1: [Pacote] = [], r2: [Pacote] = []
        do { r1 = try p1.alimentar(b) } catch { e1 = true }
        for i in 0..<b.count { if e2 { break }; do { r2 += try p2.alimentar(b.subdata(in: i..<i+1)) } catch { e2 = true } }
        if e1 != e2 || r1 != r2 { diverg += 1 }
    }
    say("  fuzz diferencial 5000 fluxos: divergencias=\(diverg)")
}

// ======== C. Servidor ========
say("=== C1. ocupado: cliente 2 sempre recebe a linha? (50x) ===")
do {
    let f = Fonte(); f.emitir = false
    let (s, porta) = subir { f }
    let a = Cli(porta: porta)!; a.enviar(hs); _ = a.linha()
    var ok = 0, perdeu = 0, motivos: [String: Int] = [:]
    for _ in 0..<50 {
        let b = Cli(porta: porta)!; b.enviar(hs)
        let (l, m) = b.linha(2)
        if let l, l.contains("ocupado") { ok += 1 } else { perdeu += 1; motivos[m, default: 0] += 1 }
        b.fechar()
    }
    say("  recebeu ocupado: \(ok)/50, perdeu: \(perdeu) \(motivos)")
    // variante: cliente manda handshake + mais bytes (ping) junto
    var ok2 = 0
    for _ in 0..<50 {
        let b = Cli(porta: porta)!; b.enviar(Data(hs.utf8) + Pacote(tipo: 9, tempo: 1, dados: Data()).serializar())
        let (l, _) = b.linha(2); if let l, l.contains("ocupado") { ok2 += 1 }; b.fechar()
    }
    say("  variante com ping colado: recebeu ocupado \(ok2)/50")
    a.fechar(); s.parar()
}

say("=== C2. dados durante 'iniciando' (camera demorando): memoria e fechamento ===")
do {
    let f = Fonte(); f.atrasoInicio = 3600
    let (s, porta) = subir { f }
    let antes = rssMB()
    let c = Cli(porta: porta)!; c.enviar(hs)
    let bloco = Data(count: 1 << 20); var mandou = 0; let t0 = Date()
    // manda ate 600 MB ou ate falhar
    var falhou = false
    c.enviar(Data()) // noop
    var fl = fcntl(c.fd, F_GETFL); fl |= O_NONBLOCK; _ = fcntl(c.fd, F_SETFL, fl)
    while mandou < 600 && Date().timeIntervalSince(t0) < 25 {
        let ok = bloco.withUnsafeBytes { p -> Int in send(c.fd, p.baseAddress!, bloco.count, 0) }
        if ok > 0 { mandou += ok / 1_048_576 > 0 ? 1 : 0 } else if errno == EAGAIN { usleep(2000) } else { falhou = true; break }
    }
    usleep(500_000)
    say("  enviou ~\(mandou) MB em \(String(format: "%.1f", Date().timeIntervalSince(t0)))s, falhou=\(falhou), RSS antes=\(Int(antes))MB depois=\(Int(rssMB()))MB")
    c.fechar(); usleep(500_000); s.parar()
}

say("=== C3. fechar durante 'iniciando' libera a camera e aceita o proximo? ===")
do {
    var criadas: [Fonte] = []
    let (s, porta) = subir { let f = Fonte(); f.atrasoInicio = 1.5; f.emitir = false; criadas.append(f); return f }
    let a = Cli(porta: porta)!; a.enviar(hs); usleep(200_000); a.fechar(); usleep(300_000)
    let b = Cli(porta: porta)!; b.enviar(hs)
    let (l, m) = b.linha(5)
    say("  apos fechar em iniciando, novo cliente -> \(l ?? "nada") \(m) | fonte1.parar=\(criadas[0].parouN) fonte1.iniciou=\(criadas[0]._iniciou)")
    b.fechar(); usleep(300_000); say("  fonte2.parar=\(criadas.count > 1 ? criadas[1].parouN : -1)"); s.parar()
}

say("=== C4. reconexao rapida 50x (fecha e reabre sem esperar) ===")
do {
    var criadas = 0, parou = 0; let tl = NSLock(); var fontes: [Fonte] = []
    let (s, porta) = subir { let f = Fonte(); tl.lock(); criadas += 1; fontes.append(f); tl.unlock(); return f }
    var ocup = 0, ok = 0, outro: [String: Int] = [:]
    for _ in 0..<50 {
        let c = Cli(porta: porta)!; c.enviar(hs); let (l, m) = c.linha(3)
        if let l { if l.contains("\"ok\":true") { ok += 1 } else if l.contains("ocupado") { ocup += 1 } else { outro[l, default: 0] += 1 } } else { outro[m, default: 0] += 1 }
        c.fechar()
    }
    usleep(800_000); tl.lock(); parou = fontes.filter { $0.parouN > 0 }.count; tl.unlock()
    say("  ok=\(ok) ocupado=\(ocup) outro=\(outro) | fontes criadas=\(criadas) paradas=\(parou)")
    s.parar()
}

say("=== C5. cliente lento que nao le mas manda ping (20s) ===")
do {
    let f = Fonte(); f.chaveBytes = 300_000; f.chaveACada = 6  // keyframe a cada 200ms ~1.5MB/s
    let (s, porta) = subir({ f }) { $0.limiteFilaBytes = 512 * 1024 }
    let c = Cli(porta: porta)!; var rcv: Int32 = 4096; setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &rcv, 4)
    c.enviar(hs); var d = Data(); var b = [UInt8](repeating: 0, count: 1); while !d.contains(0x0A) { _ = recv(c.fd, &b, 1, 0); d.append(b[0]) }
    let r0 = rssMB(); let t0 = Date(); var fechado = false
    while Date().timeIntervalSince(t0) < 20 {
        if !c.enviar(Pacote(tipo: 9, tempo: 1, dados: Data()).serializar()) { fechado = true; break }
        sleep(1)
        if Int(Date().timeIntervalSince(t0)) % 5 == 0 { say("   t=\(Int(Date().timeIntervalSince(t0)))s RSS=\(Int(rssMB()))MB emitidos=\(f.emitidos) keyframes pedidos=\(f.kfN)") }
    }
    say("  RSS inicial=\(Int(r0))MB final=\(Int(rssMB()))MB emitidos=\(f.emitidos) kf pedidos=\(f.kfN) fechou=\(fechado)")
    // retoma leitura: quanto atraso acumulou?
    var total = 0; var buf = [UInt8](repeating: 0, count: 1 << 20); var fl = fcntl(c.fd, F_GETFL); fl |= O_NONBLOCK; _ = fcntl(c.fd, F_SETFL, fl)
    let t1 = Date(); while Date().timeIntervalSince(t1) < 3 { let n = recv(c.fd, &buf, buf.count, 0); if n > 0 { total += n } else { usleep(5000) } }
    say("  ao voltar a ler em 3s recebeu \(total / 1_048_576) MB de backlog")
    c.fechar(); s.parar()
}

say("=== C6. slowloris no handshake ===")
do {
    let (s, porta) = subir({ Fonte() })
    let c = Cli(porta: porta)!; let t0 = Date(); var res = ""
    for ch in Array(hs.utf8).prefix(20) { if !c.enviar(Data([ch])) { break }; usleep(400_000); let (d, m) = c.ler(ate: 0.01); if !d.isEmpty || m != "timeout" { res = "\(String(decoding: d, as: UTF8.self)) \(m)"; break } }
    say("  fechado/respondido apos \(String(format: "%.1f", Date().timeIntervalSince(t0)))s -> \(res.trimmingCharacters(in: .whitespacesAndNewlines))")
    s.parar()
}

say("=== C7. handshake + ping colados, handshake CRLF, silencio apos handshake (5s) ===")
do {
    let f = Fonte(); f.emitir = false
    let (s, porta) = subir({ f }) { $0.timeoutInatividade = 5 }
    let c = Cli(porta: porta)!; c.enviar(Data(hs.utf8) + Pacote(tipo: 9, tempo: 777, dados: Data()).serializar())
    let (d, m) = c.ler(ate: 2) { $0.count > 200 && $0.suffix(21).first == 10 }
    var p = ParserPacotes(); let idx = d.firstIndex(of: 10)!; let rest = d[(idx+1)...]
    let pk = (try? p.alimentar(Data(rest))) ?? []
    say("  pong colado: \(pk.map { "tipo\($0.tipo)" }) (\(m))")
    // silencio
    let t0 = Date(); let (_, m2) = c.ler(ate: 9); say("  silencio -> \(m2) apos \(String(format: "%.1f", Date().timeIntervalSince(t0)))s (doc: 5s)")
    let c2 = Cli(porta: porta)!; c2.enviar("{\"v\":1}\r\n"); let (l, _) = c2.linha(3); say("  handshake CRLF -> \(l ?? "nada")")
    s.parar()
}

say("=== C8. flood de pings sem ler (memoria) ===")
do {
    let f = Fonte(); f.emitir = false
    let (s, porta) = subir({ f })
    let c = Cli(porta: porta)!; var rcv: Int32 = 4096; setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &rcv, 4)
    c.enviar(hs); var d = Data(); var b = [UInt8](repeating: 0, count: 1); while !d.contains(0x0A) { _ = recv(c.fd, &b, 1, 0); d.append(b[0]) }
    let r0 = rssMB(); let ping = Pacote(tipo: 9, tempo: 1, dados: Data()).serializar()
    var lote = Data(); for _ in 0..<10000 { lote.append(ping) }
    var fl = fcntl(c.fd, F_GETFL); fl |= O_NONBLOCK; _ = fcntl(c.fd, F_SETFL, fl)
    var enviados = 0; let t0 = Date()
    while Date().timeIntervalSince(t0) < 10 { let n = lote.withUnsafeBytes { send(c.fd, $0.baseAddress!, lote.count, 0) }; if n > 0 { enviados += n / 13 } else if errno == EAGAIN { usleep(1000) } else { break } }
    sleep(1)
    say("  enviou ~\(enviados) pings sem ler, RSS \(Int(r0))MB -> \(Int(rssMB()))MB")
    c.fechar(); s.parar()
}
say("FIM")
