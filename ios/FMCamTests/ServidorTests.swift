import Network
import XCTest
@testable import FMCam

final class FonteFalsa: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    var falhar: ErroFonte?
    var quadros = 5
    private let trava = NSLock()
    private var _keyframes = 0
    private(set) var configRecebida: ConfigVideo?
    private(set) var parada = false
    var keyframesPedidos: Int { trava.lock(); defer { trava.unlock() }; return _keyframes }

    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        configRecebida = config
        if let e = falhar { completion(.failure(e)); return }
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
        aoEvento?(.config(tempo: 1000, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
        aoEvento?(.quadro(tempo: 1000, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
        for i in 1..<max(quadros, 1) {
            aoEvento?(.quadro(tempo: UInt64(1000 + i * 33333), chave: false, dados: Data([0, 0, 0, 1, 0x41])))
        }
    }
    func parar() { trava.lock(); parada = true; trava.unlock() }
    func pedirKeyframe() { trava.lock(); _keyframes += 1; trava.unlock() }
}

/// Câmera que nunca fica pronta (ou só quando `liberar()` é chamado).
final class FonteTravada: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    private let trava = NSLock()
    private var _parada = false
    var parada: Bool { trava.lock(); defer { trava.unlock() }; return _parada }
    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {}
    func parar() { trava.lock(); _parada = true; trava.unlock() }
    func pedirKeyframe() {}
}

/// Entrega a config e um quadro-chave e depois fica muda.
final class FonteMuda: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    private let trava = NSLock()
    private var _parada = false
    var parada: Bool { trava.lock(); defer { trava.unlock() }; return _parada }
    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
        aoEvento?(.config(tempo: 1, dados: Data([0, 0, 0, 1, 0x67])))
        aoEvento?(.quadro(tempo: 1, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
    }
    func parar() { trava.lock(); _parada = true; trava.unlock() }
    func pedirKeyframe() {}
}

final class ClienteTeste {
    let conn: NWConnection
    private let trava = NSLock()
    private var recebido = Data()
    private(set) var fechado = false
    private var parser = ParserPacotes()
    private(set) var pacotes: [Pacote] = []

    init(porta: UInt16) {
        conn = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: porta)!, using: .tcp)
        conn.stateUpdateHandler = { [weak self] e in
            if case .failed = e { self?.marcarFechado() }
            if case .cancelled = e { self?.marcarFechado() }
        }
        conn.start(queue: DispatchQueue(label: "cliente-teste"))
        ler()
    }

    private func marcarFechado() { trava.lock(); fechado = true; trava.unlock() }
    var estaFechado: Bool { trava.lock(); defer { trava.unlock() }; return fechado }

    private func ler() {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] d, _, fim, erro in
            guard let self else { return }
            if let d { self.trava.lock(); self.recebido.append(d); self.trava.unlock() }
            if fim || erro != nil { self.marcarFechado(); return }
            self.ler()
        }
    }

    func enviar(_ d: Data) { conn.send(content: d, completion: .contentProcessed { _ in }) }
    func enviar(_ s: String) { enviar(Data(s.utf8)) }

    func esperar(_ segundos: TimeInterval = 5, _ cond: () -> Bool) -> Bool {
        let fim = Date().addingTimeInterval(segundos)
        while Date() < fim {
            if cond() { return true }
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        return cond()
    }

    /// Lê a linha JSON de resposta do handshake.
    func lerLinha() -> [String: Any]? {
        guard esperar(5, { trava.lock(); defer { trava.unlock() }; return recebido.contains(0x0A) }) else { return nil }
        trava.lock()
        guard let idx = recebido.firstIndex(of: 0x0A) else { trava.unlock(); return nil }
        let linha = Data(recebido[recebido.startIndex..<idx])
        recebido.removeSubrange(recebido.startIndex...idx)
        trava.unlock()
        return try? JSONSerialization.jsonObject(with: linha) as? [String: Any]
    }

    /// Junta os pacotes que chegaram até ter `n`.
    func esperarPacotes(_ n: Int, _ aceitar: (([Pacote]) -> Bool)? = nil) -> Bool {
        esperar {
            trava.lock()
            let d = recebido
            recebido = Data()
            trava.unlock()
            if !d.isEmpty { pacotes += (try? parser.alimentar(d)) ?? [] }
            return aceitar?(pacotes) ?? (pacotes.count >= n)
        }
    }
}

final class ServidorTests: XCTestCase {
    let identidade = IdentidadeAparelho(modelo: "iPhone14,5", nome: "Camera 1", id: "UUID-TESTE")
    var servidor: Servidor?

    override func tearDown() { servidor?.parar(); servidor = nil }

    func subir(fonte: @escaping () -> FonteDeVideo, ajustar: (inout Servidor.Opcoes) -> Void = { _ in }) throws -> UInt16 {
        var op = Servidor.Opcoes()
        op.porta = 0
        op.timeoutHandshake = 0.4
        ajustar(&op)
        let s = Servidor(opcoes: op, identidade: identidade, criarFonte: fonte)
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return try XCTUnwrap(s.portaAtual, "servidor não abriu porta")
    }

    func testFluxoCompleto() throws {
        let fonte = FonteFalsa()
        let porta = try subir { fonte }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1,"cliente":"teste","preset":"720p60","lente":"tele","bitrate":4000000}"# + "\n")
        let resp = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(resp["ok"] as? Bool, true)
        XCTAssertEqual(resp["w"] as? Int, 1920)
        XCTAssertEqual(resp["fps"] as? Int, 30)
        XCTAssertEqual(resp["codec"] as? String, "h264")
        XCTAssertEqual(resp["aparelho"] as? String, "iPhone14,5")
        XCTAssertEqual(resp["nome"] as? String, "Camera 1")
        XCTAssertEqual(resp["id"] as? String, "UUID-TESTE")
        XCTAssertEqual(fonte.configRecebida, ConfigVideo(preset: .p720_60, lente: .tele, bitrate: 4_000_000))

        XCTAssertTrue(c.esperarPacotes(6))
        guard c.pacotes.count >= 3 else { return XCTFail("poucos pacotes: \(c.pacotes.count)") }
        XCTAssertEqual(c.pacotes[0].tipo, 1)
        XCTAssertEqual(c.pacotes[0].tempo, 1000)
        XCTAssertEqual(c.pacotes[1].tipo, 2)
        XCTAssertEqual(c.pacotes[1].dados.first, 0x01)
        XCTAssertEqual(c.pacotes[2].dados.first, 0x00)
        XCTAssertEqual(c.pacotes.filter { $0.tipo == 2 }.count, 5)

        // ping -> pong com o tempo ecoado
        c.enviar(Pacote(tipo: 9, tempo: 777_777, dados: Data()).serializar())
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 10 } })
        let pong = try XCTUnwrap(c.pacotes.first { $0.tipo == 10 })
        XCTAssertEqual(pong.dados, Pacote.u64(777_777))
        XCTAssertGreaterThan(pong.tempo, 0)

        // comando keyframe
        c.enviar(Pacote(tipo: 4, tempo: 1, dados: Data(#"{"cmd":"keyframe"}"#.utf8)).serializar())
        XCTAssertTrue(c.esperar { fonte.keyframesPedidos == 1 })

        // segundo cliente recebe ocupado
        let c2 = ClienteTeste(porta: porta)
        let r2 = try XCTUnwrap(c2.lerLinha())
        XCTAssertEqual(r2["ok"] as? Bool, false)
        XCTAssertEqual(r2["erro"] as? String, "ocupado")
        XCTAssertTrue(c2.esperar { c2.estaFechado })

        // primeiro cliente sai: fonte para e um novo cliente entra
        c.conn.cancel()
        XCTAssertTrue(c.esperar { fonte.parada })
        let c3 = ClienteTeste(porta: porta)
        c3.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c3.lerLinha())["ok"] as? Bool, true)
    }

    func testCameraIndisponivel() throws {
        let porta = try subir {
            let f = FonteFalsa(); f.falhar = .camera("sem câmera"); return f
        }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        let r = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(r["ok"] as? Bool, false)
        XCTAssertEqual(r["erro"] as? String, "camera")
        XCTAssertTrue(c.esperar { c.estaFechado })
    }

    func testVersaoErrada() throws {
        let porta = try subir { FonteFalsa() }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":9}"# + "\n")
        let r = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(r["erro"] as? String, "versao")
        XCTAssertEqual(r["v_suportadas"] as? [Int], [1])
    }

    func testHandshakeQuebradoELongo() throws {
        let porta = try subir { FonteFalsa() }
        let a = ClienteTeste(porta: porta)
        a.enviar("{quebrado\n")
        XCTAssertEqual(try XCTUnwrap(a.lerLinha())["erro"] as? String, "handshake")
        XCTAssertTrue(a.esperar { a.estaFechado })
        let b = ClienteTeste(porta: porta)
        b.enviar(String(repeating: "a", count: 5000))
        XCTAssertEqual(try XCTUnwrap(b.lerLinha())["erro"] as? String, "handshake")
    }

    func testTimeoutDeHandshake() throws {
        let porta = try subir { FonteFalsa() }
        let c = ClienteTeste(porta: porta)
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["erro"] as? String, "handshake")
    }

    func testClienteMudoEDerrubado() throws {
        let fonte = FonteFalsa()
        let porta = try subir(fonte: { fonte }) { $0.timeoutInatividade = 0.5 }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["ok"] as? Bool, true)
        XCTAssertTrue(c.esperar(4) { c.estaFechado })
        XCTAssertTrue(c.esperar { fonte.parada })
    }

    func testStatusPeriodico() throws {
        let porta = try subir(fonte: { FonteFalsa() }) { $0.intervaloStatus = 0.2 }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        _ = c.lerLinha()
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 3 } })
        let st = try XCTUnwrap(c.pacotes.first { $0.tipo == 3 })
        let j = try XCTUnwrap(JSONSerialization.jsonObject(with: st.dados) as? [String: Any])
        XCTAssertNotNil(j["bateria"]); XCTAssertNotNil(j["temp"]); XCTAssertNotNil(j["kbps"])
    }

    /// Sem câmera (simulador) o app real responde erro camera em vez de travar.
    func testFonteRealSemCameraResponderErro() throws {
        try XCTSkipIf(Captura.dispositivo(para: .wide) != nil, "este aparelho tem câmera")
        let porta = try subir { FonteCamera(motor: CapturaCompartilhada()) }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        let r = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(r["erro"] as? String, "camera")
    }

    // MARK: recuperação

    func testCameraQueNuncaFicaProntaDaErroCameraNoLimite() throws {
        let travada = FonteTravada()
        var primeira = true
        let porta = try subir(fonte: {
            if primeira { primeira = false; return travada }
            return FonteFalsa()
        }) { $0.timeoutPartidaCamera = 0.4 }
        let c = ClienteTeste(porta: porta)
        let t0 = Date()
        c.enviar(#"{"v":1}"# + "\n")
        let r = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(r["erro"] as? String, "camera")
        XCTAssertLessThan(Date().timeIntervalSince(t0), 2)
        XCTAssertTrue(c.esperar { travada.parada }, "fonte deve ser parada")
        // e o próximo cliente é atendido, sem "ocupado"
        let c2 = ClienteTeste(porta: porta)
        c2.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c2.lerLinha())["ok"] as? Bool, true)
    }

    func testClienteSaiDuranteIniciandoParaAFonteELiberaVaga() throws {
        let travada = FonteTravada()
        var primeira = true
        let porta = try subir(fonte: {
            if primeira { primeira = false; return travada }
            return FonteFalsa()
        }) { $0.timeoutPartidaCamera = 30 }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        c.conn.cancel()
        XCTAssertTrue(c.esperar { travada.parada }, "fonte deve parar quando o cliente sai no iniciando")
        let c2 = ClienteTeste(porta: porta)
        c2.enviar(#"{"v":1}"# + "\n")
        let r = try XCTUnwrap(c2.lerLinha())
        XCTAssertEqual(r["ok"] as? Bool, true, "não pode vir ocupado: \(r)")
    }

    func testWatchdogEncerraSessaoQuandoQuadrosPararam() throws {
        let muda = FonteMuda()
        var primeira = true
        let porta = try subir(fonte: {
            if primeira { primeira = false; return muda }
            return FonteFalsa()
        }) { $0.timeoutSemQuadros = 0.5; $0.timeoutInatividade = 30 }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["ok"] as? Bool, true)
        let t0 = Date()
        XCTAssertTrue(c.esperar(4) { c.estaFechado }, "watchdog deve fechar a conexão")
        XCTAssertLessThan(Date().timeIntervalSince(t0), 3)
        XCTAssertTrue(c.esperar { muda.parada })
        let c2 = ClienteTeste(porta: porta)
        c2.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c2.lerLinha())["ok"] as? Bool, true)
    }
}
