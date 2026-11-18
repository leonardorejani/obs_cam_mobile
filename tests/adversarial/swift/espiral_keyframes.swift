import Foundation
import Darwin
func say(_ s: String) { print(s); fflush(stdout) }
final class Fonte: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    let t = NSLock(); var _kf = 0, _chavesEnviadas = 0, _deltasEnviados = 0, _deltasDescartaveis = 0
    var timer: DispatchSourceTimer?; var chaveBytes = Int(CommandLine.arguments[2])!, deltaBytes = Int(CommandLine.arguments[3])!
    var forcar = false
    func iniciar(_ c: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
        aoEvento?(.config(tempo: 1000, dados: Data([0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xCE])))
        var i = 0
        let tm = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "f")); tm.schedule(deadline: .now(), repeating: 1.0/30)
        tm.setEventHandler { [self] in
            t.lock(); let f = forcar; forcar = false; t.unlock()
            let chave = i % 60 == 0 || f
            t.lock(); if chave { _chavesEnviadas += 1 } else { _deltasEnviados += 1 }; t.unlock()
            aoEvento?(.quadro(tempo: UInt64(1000 + i*33333), chave: chave, dados: Data(count: chave ? chaveBytes : deltaBytes))); i += 1
        }
        tm.resume(); timer = tm
    }
    func parar() { timer?.cancel() }
    func pedirKeyframe() { t.lock(); _kf += 1; forcar = true; t.unlock() }
}
var op = Servidor.Opcoes(); op.porta = 0
let f = Fonte()
let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "m", nome: "n", id: "i"), criarFonte: { f })
s.iniciar(); while s.portaAtual == nil { usleep(10000) }
let fd = socket(AF_INET, SOCK_STREAM, 0)
var a = sockaddr_in(); a.sin_len = 16; a.sin_family = sa_family_t(AF_INET); a.sin_port = s.portaAtual!.bigEndian; a.sin_addr.s_addr = inet_addr("127.0.0.1")
var rc: Int32 = 32768; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rc, 4)
_ = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, 16) } }
let hs = Array("{\"v\":1}\n".utf8); _ = send(fd, hs, hs.count, 0)
var parser = ParserPacotes(); var chaves = 0, deltas = 0, bytes = 0; var buf = [UInt8](repeating: 0, count: 50_000)
var got = Data(); var hsDone = false
let rateMBs = Double(CommandLine.arguments.dropFirst().first ?? "5")!
let t0 = Date()
while Date().timeIntervalSince(t0) < 6 {
    let n = recv(fd, &buf, buf.count, 0); if n <= 0 { break }
    bytes += n; var d = Data(buf[0..<n])
    if !hsDone { if let i = d.firstIndex(of: 10) { d = Data(d[(i+1)...]); hsDone = true } else { continue } }
    for p in (try? parser.alimentar(d)) ?? [] where p.tipo == 2 { if p.dados.first == 1 { chaves += 1 } else { deltas += 1 } }
    usleep(UInt32(Double(n) / (rateMBs * 1e6) * 1e6))
}
let secs = Date().timeIntervalSince(t0)
f.t.lock(); let kfs = f._kf, ec = f._chavesEnviadas, ed = f._deltasEnviados; f.t.unlock()
say(String(format: "leitor a %.0f MB/s (%.0f Mbps), fonte ~%.0f Mbps media: em %.0fs fonte gerou %d chaves + %d deltas, keyframes pedidos pelo servidor=%d; cliente recebeu %d chaves + %d deltas (%.1f MB)", rateMBs, rateMBs*8, (Double(ec)*1e6 + Double(ed)*1e5)*8/secs/1e6, secs, ec, ed, kfs, chaves, deltas, Double(bytes)/1e6))
exit(0)
