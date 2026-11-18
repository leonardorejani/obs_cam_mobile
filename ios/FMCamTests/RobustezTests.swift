import Darwin
import Network
import XCTest
@testable import FMCam

/// Cliente de socket cru (BSD): controla o buffer de leitura e não lê quando não quer.
final class ClienteCru {
    let fd: Int32
    init?(porta: UInt16, rcvbuf: Int32? = nil) {
        fd = socket(AF_INET, SOCK_STREAM, 0)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, 4)
        if var r = rcvbuf { setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &r, 4) }
        var a = sockaddr_in()
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        a.sin_family = sa_family_t(AF_INET)
        a.sin_port = porta.bigEndian
        a.sin_addr.s_addr = inet_addr("127.0.0.1")
        let r = withUnsafePointer(to: &a) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        if r != 0 { close(fd); return nil }
    }
    deinit { close(fd) }

    @discardableResult func enviar(_ d: Data) -> Bool {
        d.withUnsafeBytes { p in
            var off = 0
            while off < d.count {
                let n = send(fd, p.baseAddress! + off, d.count - off, 0)
                if n <= 0 { return false }
                off += n
            }
            return true
        }
    }
    @discardableResult func enviar(_ s: String) -> Bool { enviar(Data(s.utf8)) }

    /// Lê até `seg` segundos ou até `parar` aceitar. Devolve bytes e motivo ("ok", "timeout", "EOF", "erro").
    func ler(ate seg: Double, parar: ((Data) -> Bool)? = nil) -> (Data, String) {
        var saida = Data()
        let fim = Date().addingTimeInterval(seg)
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let resto = fim.timeIntervalSinceNow
            if resto <= 0 { return (saida, "timeout") }
            var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            if poll(&p, 1, Int32(min(resto * 1000, 100))) == 0 { continue }
            let n = recv(fd, &buf, buf.count, 0)
            if n == 0 { return (saida, "EOF") }
            if n < 0 { return (saida, "erro \(errno)") }
            saida.append(buf, count: n)
            if let f = parar, f(saida) { return (saida, "ok") }
        }
    }

    func lerLinha(_ seg: Double = 5) -> [String: Any]? {
        let (d, _) = ler(ate: seg) { $0.contains(0x0A) }
        guard let i = d.firstIndex(of: 0x0A) else { return nil }
        return try? JSONSerialization.jsonObject(with: d[d.startIndex..<i]) as? [String: Any]
    }

    /// Fecha de verdade a conexão? (true se o servidor já fechou)
    func servidorFechou(em seg: Double) -> Bool {
        let (_, m) = ler(ate: seg)
        return m == "EOF" || m.hasPrefix("erro")
    }
}

/// Emite quadros num timer: chave grande a cada `chaveACada` e deltas pequenos.
final class FonteRapida: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    var chaveBytes = 700_000, deltaBytes = 20_000, chaveACada = 6
    private let trava = NSLock()
    private var _pedidos = 0
    private var timer: DispatchSourceTimer?
    var pedidos: Int { trava.lock(); defer { trava.unlock() }; return _pedidos }
    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
        aoEvento?(.config(tempo: 1, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
        var i = 0
        let t = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "fonte-rapida"))
        t.schedule(deadline: .now(), repeating: 1.0 / 30)
        t.setEventHandler { [weak self] in
            guard let self else { return }
            let chave = i % self.chaveACada == 0
            self.aoEvento?(.quadro(tempo: UInt64(1000 + i * 33333), chave: chave,
                                   dados: Data(count: chave ? self.chaveBytes : self.deltaBytes)))
            i += 1
        }
        t.resume()
        timer = t
    }
    func parar() { timer?.cancel() }
    func pedirKeyframe() { trava.lock(); _pedidos += 1; trava.unlock() }
}

/// Fonte que, como o codificador de verdade, lê e chama `aoEvento` de outra thread o tempo todo.
final class FonteComThread: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    private let trava = NSLock()
    private var _rodando = false
    private var _parou = false
    var parou: Bool { trava.lock(); defer { trava.unlock() }; return _parou }
    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
        trava.lock(); _rodando = true; trava.unlock()
        Thread.detachNewThread { [weak self] in
            var i = 0
            while let s = self, s.rodando {
                s.aoEvento?(.quadro(tempo: UInt64(i), chave: i == 0, dados: Data([0, 0, 0, 1, 0x65])))
                i += 1
                usleep(1000)
            }
        }
    }
    private var rodando: Bool { trava.lock(); defer { trava.unlock() }; return _rodando }
    func parar() { trava.lock(); _rodando = false; _parou = true; trava.unlock() }
    func pedirKeyframe() {}
}

final class RobustezTests: XCTestCase {
    let identidade = IdentidadeAparelho(modelo: "m", nome: "n", id: "i")
    var servidor: Servidor?
    override func tearDown() { servidor?.parar(); servidor = nil }

    func subir(fonte: @escaping () -> FonteDeVideo, ajustar: (inout Servidor.Opcoes) -> Void = { _ in }) throws -> UInt16 {
        var op = Servidor.Opcoes()
        op.porta = 0
        op.timeoutHandshake = 0.4
        op.bonjour = false
        ajustar(&op)
        let s = Servidor(opcoes: op, identidade: identidade, criarFonte: fonte)
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { usleep(10_000) }
        return try XCTUnwrap(s.portaAtual)
    }

    // MARK: 1. fila de envio

    func testClienteLentoNaoGeraEspiralDeKeyframesNemAcumula() throws {
        let fonte = FonteRapida()
        let porta = try subir(fonte: { fonte }) { $0.limiteFilaBytes = 512 * 1024 }
        let c = try XCTUnwrap(ClienteCru(porta: porta, rcvbuf: 4096))
        c.enviar(#"{"v":1}"# + "\n")
        _ = c.ler(ate: 3) { $0.contains(0x0A) }
        // pinga mas não lê: a fila do servidor enche
        let t0 = Date()
        while Date().timeIntervalSince(t0) < 4 {
            XCTAssertTrue(c.enviar(Pacote(tipo: 9, tempo: 1, dados: Data()).serializar()), "servidor fechou a conexão")
            usleep(300_000)
        }
        let pico = servidor?.picoBytesNaFila ?? -1
        XCTAssertLessThanOrEqual(fonte.pedidos, 5, "no máximo 1 pedido de keyframe por segundo")
        XCTAssertLessThanOrEqual(pico, 4 * 1024 * 1024, "fila não pode acumular: pico \(pico)")
    }

    // MARK: 2. corrida em aoEvento (rodar com -enableThreadSanitizer YES)

    func testEncerrarNaoCompeteComAThreadDoCodificador() throws {
        let fonte = FonteComThread()
        let porta = try subir(fonte: { fonte })
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertNotNil(c.lerLinha())
        usleep(200_000)
        close(c.fd)
        let fim = Date().addingTimeInterval(3)
        while !fonte.parou && Date() < fim { usleep(10_000) }
        XCTAssertTrue(fonte.parou)
        usleep(200_000)
    }

    // MARK: 3. fase iniciando

    func testIniciandoSemDadosFechaPeloTimeoutDeInatividade() throws {
        let travada = FonteTravada()
        let porta = try subir(fonte: { travada }) { $0.timeoutPartidaCamera = 30; $0.timeoutInatividade = 0.5 }
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertTrue(c.servidorFechou(em: 3), "deveria fechar por inatividade durante o iniciando")
        XCTAssertTrue(travada.parada)
    }

    func testIniciandoComLixoDemaisFecha() throws {
        let travada = FonteTravada()
        let porta = try subir(fonte: { travada }) { $0.timeoutPartidaCamera = 30; $0.timeoutInatividade = 30 }
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(Data(#"{"v":1}"#.utf8) + Data([0x0A]) + Data(count: 100 * 1024))
        XCTAssertTrue(c.servidorFechou(em: 3), "mais de 64 KiB durante o iniciando deve fechar")
        XCTAssertTrue(travada.parada)
    }

    // MARK: 4. ocupado só depois de ler a linha

    func testOcupadoLeALinhaAntesDeResponder() throws {
        let porta = try subir(fonte: { FonteFalsa() }) { $0.timeoutHandshake = 2 }
        let c1 = try XCTUnwrap(ClienteCru(porta: porta))
        c1.enviar(#"{"v":1}"# + "\n")
        XCTAssertNotNil(c1.lerLinha())
        let c2 = try XCTUnwrap(ClienteCru(porta: porta))
        let (antes, _) = c2.ler(ate: 0.4)
        XCTAssertTrue(antes.isEmpty, "não pode responder antes de ler a linha do cliente")
        c2.enviar(#"{"v":1}"# + "\n")
        let (d, m) = c2.ler(ate: 3) { $0.contains(0x0A) }
        XCTAssertEqual(m, "ok")
        XCTAssertTrue(String(decoding: d, as: UTF8.self).contains("ocupado"))
        XCTAssertTrue(c2.servidorFechou(em: 3))
    }

    func testOcupadoSemLinhaResponderNoTimeout() throws {
        let porta = try subir(fonte: { FonteFalsa() }) { $0.timeoutHandshake = 0.5 }
        let c1 = try XCTUnwrap(ClienteCru(porta: porta))
        c1.enviar(#"{"v":1}"# + "\n")
        XCTAssertNotNil(c1.lerLinha())
        let c2 = try XCTUnwrap(ClienteCru(porta: porta))
        let (d, _) = c2.ler(ate: 3) { $0.contains(0x0A) }
        XCTAssertTrue(String(decoding: d, as: UTF8.self).contains("ocupado"))
    }

    // MARK: 5. regras do texto

    func linha(_ s: String) -> Data { Data(s.utf8) }

    func testChaveRepetidaEHandshake() {
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":1,"v":2}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":1,"v":1}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":1,"x":{"a":1,"a":2}}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":1,"preset":"720p30","preset":"720p60"}"#)), .invalido)
    }

    func testBomEUtf8InvalidoEHandshake() {
        XCTAssertEqual(Handshake.interpretar(linha: Data([0xEF, 0xBB, 0xBF]) + linha(#"{"v":1}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: Data(#"{"v":1,"cliente":""#.utf8) + Data([0xFF, 0xFE]) + Data(#""}"#.utf8)), .invalido)
    }

    func testLimite4096SemContarQuebraDeLinha() {
        func comTamanho(_ n: Int) -> Data {
            let base = #"{"v":1,"cliente":""#, fim = #""}"#
            return linha(base + String(repeating: "a", count: n - base.utf8.count - fim.utf8.count) + fim)
        }
        func aceito(_ r: ResultadoHandshake) -> Bool { if case .aceito = r { return true }; return false }
        XCTAssertTrue(aceito(Handshake.interpretar(linha: comTamanho(4096))))
        XCTAssertEqual(Handshake.interpretar(linha: comTamanho(4097)), .invalido)
    }

    func testVPrecisaSerInteiro() {
        for ruim in [#"{"v":1.0}"#, #"{"v":1e0}"#, #"{"v":1.5}"#, #"{"v":"1"}"#, #"{"v":null}"#, #"{"v":true}"#,
                     #"{"v":[1]}"#, #"{}"#, #"{"cliente":"x"}"#, #"{"v":1e15}"#, #"{"v":1e400}"#] {
            XCTAssertEqual(Handshake.interpretar(linha: linha(ruim)), .invalido, ruim)
        }
    }

    func testVInteiroForaDasSuportadasEVersao() {
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":0}"#)), .versaoNaoSuportada(0))
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":-1}"#)), .versaoNaoSuportada(-1))
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":2}"#)), .versaoNaoSuportada(2))
        if case .versaoNaoSuportada = Handshake.interpretar(linha: linha(#"{"v":99999999999999999999999}"#)) {} else {
            XCTFail("inteiro enorme deve ser versao")
        }
    }

    func testCampoDesconhecidoDeQualquerTipoEIgnorado() {
        for extra in [#""x":[1,2,{"a":1}]"#, #""x":{"a":{"b":null}}"#, #""x":3.5e10"#, #""x":null"#, #""x":"txt""#, #""x":false"#] {
            let r = Handshake.interpretar(linha: linha("{\"v\":1,\(extra)}"))
            if case .aceito = r {} else { XCTFail("deveria aceitar com \(extra): \(r)") }
        }
    }

    func testJsonMalFormadoEInvalido() {
        for ruim in [#"{"v":1,}"#, #"{"v":1} x"#, #"{'v':1}"#, #"[1]"#, "", #"{"v":01}"#, #"{"v":1 // c"#] {
            XCTAssertEqual(Handshake.interpretar(linha: linha(ruim)), .invalido, ruim)
        }
        XCTAssertEqual(Handshake.interpretar(linha: linha(String(repeating: "[", count: 5000))), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":1}"# + "\r")), .aceito(PedidoHandshake(v: 1, cliente: "", preset: .p1080_30, lente: .wide, bitrate: 10_000_000)))
    }

    func testHandshakeComBomViaSocket() throws {
        let porta = try subir(fonte: { FonteFalsa() })
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(Data([0xEF, 0xBB, 0xBF]) + Data("{\"v\":1}\n".utf8))
        XCTAssertEqual(c.lerLinha()?["erro"] as? String, "handshake")
    }

    // MARK: 6. partida cancelada

    func testPartidaCanceladaNaoLiga() {
        XCTAssertFalse(ControleDePartida.deveProsseguir(emPrimeiroPlano: false, cancelada: true))
        XCTAssertTrue(ControleDePartida.deveProsseguir(emPrimeiroPlano: true, cancelada: true), "em primeiro plano a câmera fica ligada de qualquer jeito")
        XCTAssertTrue(ControleDePartida.deveProsseguir(emPrimeiroPlano: false, cancelada: false))
    }
}

extension RobustezTests {
    func testTetoDaFilaFechaAConexao() throws {
        let fonte = FonteRapida()
        fonte.chaveBytes = 300_000
        let porta = try subir(fonte: { fonte }) { $0.tetoFilaBytes = 200_000 }
        let c = try XCTUnwrap(ClienteCru(porta: porta, rcvbuf: 4096))
        c.enviar(#"{"v":1}"# + "\n")
        _ = c.ler(ate: 3) { $0.contains(0x0A) }
        XCTAssertTrue(c.servidorFechou(em: 4), "fila acima do teto deve fechar a conexão")
    }
}
