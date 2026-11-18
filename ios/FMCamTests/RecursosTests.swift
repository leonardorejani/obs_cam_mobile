import CoreMedia
import CoreVideo
import Network
import XCTest
@testable import FMCam

// MARK: - Ajustes, conflito e persistência

final class AjustesTests: XCTestCase {
    func defaults() -> UserDefaults {
        let n = "teste-ajustes-\(UUID().uuidString)"
        let d = UserDefaults(suiteName: n)!
        d.removePersistentDomain(forName: n)
        return d
    }

    func testPadrao() {
        let a = AjustesCamera()
        XCTAssertEqual(a.lente, .wide)
        XCTAssertEqual(a.zoom, 1)
        XCTAssertFalse(a.estabilizar, "estabilização desligada por padrão (latência)")
        XCTAssertTrue(a.grade)
        XCTAssertFalse(a.centro)
    }

    func testPersistenciaIdaEVolta() {
        let d = defaults()
        var a = AjustesCamera()
        a.lente = .tele; a.zoom = 2.5; a.focoTravado = true; a.exposicaoTravada = true
        a.brancoTravado = true; a.kelvin = 4300; a.bias = -1.3; a.estabilizar = true; a.grade = false; a.centro = true
        a.salvar(defaults: d)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d), a)
    }

    func testSemSalvoOuCorrompidoVoltaAoPadrao() {
        let d = defaults()
        XCTAssertEqual(AjustesCamera.carregar(defaults: d), AjustesCamera())
        d.set(Data("lixo{".utf8), forKey: AjustesCamera.chave)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d), AjustesCamera())
    }

    func testJsonAntigoComCamposFaltandoUsaPadraoNoResto() {
        let d = defaults()
        d.set(Data(#"{"lente":"ultra","bias":0.5}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.lente, .ultra)
        XCTAssertEqual(a.bias, 0.5)
        XCTAssertEqual(a.zoom, 1)
        XCTAssertTrue(a.grade)
    }

    func testValoresForaDaFaixaSaoLimitados() {
        let d = defaults()
        d.set(Data(#"{"zoom":99,"bias":-50,"kelvin":10,"lente":"xx"}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.zoom, AjustesCamera.faixaZoom.upperBound)
        XCTAssertEqual(a.bias, AjustesCamera.faixaBias.lowerBound)
        XCTAssertEqual(a.kelvin, AjustesCamera.faixaKelvin.lowerBound)
        XCTAssertEqual(a.lente, .wide)
    }

    func testPedidoDoClienteVale_UltimoQueMudouGanha() {
        var a = AjustesCamera()
        // iPhone mudou para tele 1x com zoom
        a.lente = .tele; a.zoom = 1.5
        // OBS conecta pedindo wide: o pedido vale e o zoom zera (lente diferente)
        a = a.aplicandoPedido(lente: .wide)
        XCTAssertEqual(a.lente, .wide)
        XCTAssertEqual(a.zoom, 1)
        // mesma lente: mantém o zoom do iPhone
        a.zoom = 2
        XCTAssertEqual(a.aplicandoPedido(lente: .wide).zoom, 2)
        // depois da conexão, mudança no iPhone vale ao vivo
        a.lente = .ultra
        XCTAssertEqual(a.lente, .ultra)
        // e um novo pedido do OBS (outra conexão) volta a mandar
        XCTAssertEqual(a.aplicandoPedido(lente: .tele).lente, .tele)
    }

    func testCaixaDeAjustesEntreThreads() {
        let c = CaixaAjustes()
        var a = AjustesCamera(); a.lente = .tele
        c.valor = a
        XCTAssertEqual(c.valor.lente, .tele)
    }
}

// MARK: - Lentes disponíveis

final class LentesTests: XCTestCase {
    // campos de visão horizontais típicos (graus)
    let ultra = InfoLente(lente: .ultra, fov: 120)
    let wide = InfoLente(lente: .wide, fov: 69)

    func rotulos(_ i: [InfoLente]) -> [String] { OpcoesLente.montar(i).map(\.rotulo) }

    func testIphone15ProMaxTemMeioUmDoisCinco() {
        let tele5 = InfoLente(lente: .tele, fov: 15.6)
        let o = OpcoesLente.montar([ultra, wide, tele5])
        XCTAssertEqual(o.map(\.rotulo), ["0,5x", "1x", "2x", "5x"])
        guard o.count == 4 else { return XCTFail("esperava 4 botões, vieram \(o.count)") }
        XCTAssertEqual(o[2].lente, .wide); XCTAssertEqual(o[2].zoom, 2, "2x é crop da grande-angular")
        XCTAssertEqual(o[3].lente, .tele); XCTAssertEqual(o[3].zoom, 1)
    }

    func testAparelhoComTeleCurtoNaoDuplicaDoisX() {
        let tele2 = InfoLente(lente: .tele, fov: 38.8)
        let o = OpcoesLente.montar([ultra, wide, tele2])
        XCTAssertEqual(o.map(\.rotulo), ["0,5x", "1x", "2x"])
        guard o.count == 3 else { return XCTFail("esperava 3 botões, vieram \(o.count)") }
        XCTAssertEqual(o[2].lente, .tele)
    }

    func testSemTeleTemCropDoisX() {
        XCTAssertEqual(rotulos([ultra, wide]), ["0,5x", "1x", "2x"])
        XCTAssertEqual(rotulos([wide]), ["1x", "2x"])
    }

    func testTele3x() {
        let tele3 = InfoLente(lente: .tele, fov: 24)
        XCTAssertEqual(rotulos([ultra, wide, tele3]), ["0,5x", "1x", "2x", "3x"])
    }

    func testSemCameraNaoTemBotoes() {
        XCTAssertEqual(rotulos([]), [])
    }

    func testRotulosEmPortugues() {
        XCTAssertEqual(OpcoesLente.rotulo(0.5), "0,5x")
        XCTAssertEqual(OpcoesLente.rotulo(2.5), "2,5x")
        XCTAssertEqual(OpcoesLente.rotulo(5.04), "5x")
    }

    func testFatorAtualMultiplicaZoom() {
        var a = AjustesCamera(); a.lente = .tele; a.zoom = 2
        let infos = [ultra, wide, InfoLente(lente: .tele, fov: 15.6)]
        XCTAssertEqual(OpcoesLente.fatorAtual(a, infos: infos), 10, accuracy: 0.6)
    }
}

// MARK: - Rede e Bonjour

final class RedeTests: XCTestCase {
    func testEscolheOWifiEn0() {
        let l = [InterfaceRede(nome: "lo0", ipv4: "127.0.0.1"), InterfaceRede(nome: "pdp_ip0", ipv4: "10.9.9.9"),
                 InterfaceRede(nome: "en0", ipv4: "192.168.0.12"), InterfaceRede(nome: "bridge100", ipv4: "172.20.10.1")]
        XCTAssertEqual(EnderecoRede.escolherWifi(l), "192.168.0.12")
    }

    func testSemWifiOuLinkLocalNaoServe() {
        XCTAssertNil(EnderecoRede.escolherWifi([]))
        XCTAssertNil(EnderecoRede.escolherWifi([InterfaceRede(nome: "lo0", ipv4: "127.0.0.1"), InterfaceRede(nome: "pdp_ip0", ipv4: "10.9.9.9")]))
        XCTAssertNil(EnderecoRede.escolherWifi([InterfaceRede(nome: "en0", ipv4: "169.254.3.4")]))
    }

    func testFormatoDoEndereco() {
        XCTAssertEqual(EnderecoRede.formatar(ip: "192.168.0.12", porta: 4848), "Wi-Fi: 192.168.0.12:4848")
        XCTAssertEqual(EnderecoRede.formatar(ip: nil, porta: 4848), "Wi-Fi: desligado (use o cabo)")
        XCTAssertEqual(EnderecoRede.enderecoParaCopiar(ip: "10.0.0.5", porta: 4848), "10.0.0.5:4848")
        XCTAssertNil(EnderecoRede.enderecoParaCopiar(ip: nil, porta: 4848))
    }

    func testOrigemCaboOuWifi() {
        for h in ["127.0.0.1", "127.1.2.3", "::1", "::ffff:127.0.0.1", "::1%lo0", "localhost"] {
            XCTAssertEqual(EnderecoRede.origem(host: h), .cabo, h)
        }
        for h in ["192.168.0.7", "10.0.0.2", "fe80::1%en0", "169.254.1.1"] {
            XCTAssertEqual(EnderecoRede.origem(host: h), .wifi, h)
        }
    }

    func testListarDevolveAlgoNoSimulador() {
        XCTAssertTrue(EnderecoRede.listar().contains { $0.nome == "lo0" && $0.ipv4 == "127.0.0.1" })
    }

    func testTxtDoBonjour() {
        XCTAssertEqual(Bonjour.txt(id: "ABC-123"), ["id": "ABC-123", "v": "1"])
        let s = Bonjour.servico(nome: "Palco 1", id: "ABC-123")
        XCTAssertEqual(s.name, "Palco 1")
        XCTAssertEqual(s.type, "_fmcam._tcp")
        XCTAssertEqual(NWTXTRecord(Bonjour.txt(id: "ABC-123")).dictionary, ["id": "ABC-123", "v": "1"])
    }

    func testNomeDoServicoLimitado() {
        XCTAssertEqual(Bonjour.nomeServico("  "), "OBS Cam Mobile")
        XCTAssertEqual(Bonjour.nomeServico(String(repeating: "a", count: 100)).count, 63)
    }
}

// MARK: - Fonte de câmera sobre motor falso

final class MotorFalso: MotorDeCaptura {
    private let trava = NSLock()
    private(set) var preparos = 0
    var segurar = false
    private var pendente: (() -> Void)?
    private var _amostra: ((CMSampleBuffer) -> Void)?
    private var _reconfig: ((DimensoesVideo) -> Void)?
    private var _dono: AnyObject?
    var dim = DimensoesVideo(largura: 640, altura: 360, fps: 30)

    func prepararParaTransmitir(_ config: ConfigVideo, cancelada: @escaping () -> Bool,
                                completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); preparos += 1; trava.unlock()
        let d = dim
        let r = { completion(cancelada() ? .failure(.camera("cancelada")) : .success(d)) }
        if segurar { pendente = r } else { r() }
    }
    func liberar() { let p = pendente; pendente = nil; p?() }
    func definirConsumidor(dono: AnyObject, amostra: @escaping (CMSampleBuffer) -> Void,
                           aoFalha: @escaping (String) -> Void, aoReconfigurar: @escaping (DimensoesVideo) -> Void) {
        trava.lock(); _amostra = amostra; _reconfig = aoReconfigurar; _dono = dono; trava.unlock()
    }
    func liberarConsumidor(dono: AnyObject) {
        trava.lock(); if _dono === dono { _amostra = nil; _reconfig = nil; _dono = nil }; trava.unlock()
    }
    var temConsumidor: Bool { trava.lock(); defer { trava.unlock() }; return _amostra != nil }
    func entregar(_ a: CMSampleBuffer?) {
        guard let a else { return }
        trava.lock(); let f = _amostra; trava.unlock(); f?(a)
    }
    func reconfigurar(_ d: DimensoesVideo) { trava.lock(); let f = _reconfig; trava.unlock(); f?(d) }
}

func amostraSintetica(_ i: Int, w: Int = 640, h: Int = 360) -> CMSampleBuffer? {
    var pb: CVPixelBuffer?
    CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, nil, &pb)
    guard let buf = pb else { return nil }
    CVPixelBufferLockBaseAddress(buf, [])
    memset(CVPixelBufferGetBaseAddress(buf), Int32(i * 17 % 255), CVPixelBufferGetDataSize(buf))
    CVPixelBufferUnlockBaseAddress(buf, [])
    var fd: CMVideoFormatDescription?
    CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: buf, formatDescriptionOut: &fd)
    guard let fd else { return nil }
    var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: CMTime(value: Int64(i), timescale: 30), decodeTimeStamp: .invalid)
    var sb: CMSampleBuffer?
    CMSampleBufferCreateReadyWithImageBuffer(allocator: nil, imageBuffer: buf, formatDescription: fd, sampleTiming: &timing, sampleBufferOut: &sb)
    return sb
}

final class FonteCameraTests: XCTestCase {
    final class Coletor {
        private let trava = NSLock()
        private var _eventos: [EventoVideo] = []
        func adicionar(_ e: EventoVideo) { trava.lock(); _eventos.append(e); trava.unlock() }
        var eventos: [EventoVideo] { trava.lock(); defer { trava.unlock() }; return _eventos }
        var configs: Int { eventos.filter { if case .config = $0 { return true }; return false }.count }
        var quadros: [(Bool, Data)] { eventos.compactMap { if case .quadro(_, let c, let d) = $0 { return (c, d) }; return nil } }
    }

    func iniciar(_ f: FonteCamera) -> Result<DimensoesVideo, ErroFonte>? {
        var r: Result<DimensoesVideo, ErroFonte>?
        f.iniciar(ConfigVideo()) { r = $0 }
        return r
    }

    func testSemClienteNaoCodifica() {
        let motor = MotorFalso()
        XCTAssertFalse(motor.temConsumidor, "sem cliente ninguém recebe quadros")
        let f = FonteCamera(motor: motor)
        XCTAssertNotNil(iniciar(f))
        XCTAssertTrue(motor.temConsumidor)
        f.parar()
        XCTAssertFalse(motor.temConsumidor, "ao sair o cliente a codificação para")
    }

    func testSessoesSeguidasUsamAMesmaCapturaSemRecriar() {
        let motor = MotorFalso()
        for _ in 0..<3 {
            let f = FonteCamera(motor: motor)
            guard case .success(let d)? = iniciar(f) else { return XCTFail() }
            XCTAssertEqual(d, motor.dim)
            XCTAssertTrue(motor.temConsumidor)
            f.parar()
            XCTAssertFalse(motor.temConsumidor)
        }
        XCTAssertEqual(motor.preparos, 3)
    }

    func testFluxoConfigDepoisQuadrosComPrimeiroChave() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        for i in 0..<4 { motor.entregar(amostraSintetica(i)) }
        f.parar()
        guard case .config? = col.eventos.first else { return XCTFail("config primeiro") }
        XCTAssertEqual(col.quadros.count, 4)
        guard let q0 = col.quadros.first else { return XCTFail("nenhum quadro") }
        XCTAssertTrue(q0.0)
        XCTAssertTrue(col.quadros.allSatisfy { $0.1.prefix(4) == Data([0, 0, 0, 1]) })
    }

    func testTrocaDeLenteReenviaConfigEForcaChave() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        for i in 0..<3 { motor.entregar(amostraSintetica(i)) }
        f.esvaziarCodificador()
        motor.reconfigurar(motor.dim)
        motor.entregar(amostraSintetica(3))
        f.parar()
        XCTAssertEqual(col.configs, 2, "tipo 1 de novo depois da troca")
        XCTAssertEqual(col.quadros.count, 4)
        guard col.quadros.count == 4 else { return XCTFail("quadros: \(col.quadros.count)") }
        XCTAssertTrue(col.quadros[3].0, "quadro depois da troca é chave")
    }

    func testTrocaDeLenteComOutroTamanhoRecriaOCodificador() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        motor.entregar(amostraSintetica(0))
        f.esvaziarCodificador()
        let nova = DimensoesVideo(largura: 320, altura: 180, fps: 30)
        motor.reconfigurar(nova)
        motor.entregar(amostraSintetica(1, w: 320, h: 180))
        f.parar()
        XCTAssertEqual(col.configs, 2)
        XCTAssertEqual(col.quadros.count, 2)
        guard col.quadros.count == 2 else { return XCTFail("quadros: \(col.quadros.count)") }
        XCTAssertTrue(col.quadros[1].0)
    }

    func testPararDuranteOPreparoNaoLigaNada() {
        let motor = MotorFalso()
        motor.segurar = true
        let f = FonteCamera(motor: motor)
        var r: Result<DimensoesVideo, ErroFonte>?
        f.iniciar(ConfigVideo()) { r = $0 }
        f.parar()
        motor.liberar()
        guard case .failure? = r else { return XCTFail("deveria falhar: \(String(describing: r))") }
        XCTAssertFalse(motor.temConsumidor, "cliente já saiu: nada de consumidor")
    }
}

// MARK: - Status com os ajustes

final class StatusAjustesTests: XCTestCase {
    func testStatusTrazOsAjustesAtuais() throws {
        var a = AjustesCamera()
        a.lente = .tele; a.focoTravado = true; a.exposicaoTravada = true; a.brancoTravado = true; a.bias = 0.7
        var op = Servidor.Opcoes()
        op.porta = 0; op.intervaloStatus = 0.2; op.bonjour = false
        let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "m", nome: "n", id: "i"),
                         ajustes: CaixaAjustes(a), criarFonte: { FonteFalsa() })
        s.iniciar()
        defer { s.parar() }
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { usleep(10_000) }
        let c = try XCTUnwrap(ClienteCru(porta: try XCTUnwrap(s.portaAtual)))
        c.enviar(#"{"v":1}"# + "\n")
        let (d, _) = c.ler(ate: 3) { $0.contains(0x0A) }
        let idx = try XCTUnwrap(d.firstIndex(of: 0x0A))
        var parser = ParserPacotes()
        var pacotes = try parser.alimentar(Data(d[(idx + 1)...]))
        let t0 = Date()
        while !pacotes.contains(where: { $0.tipo == 3 }) && Date().timeIntervalSince(t0) < 3 {
            let (mais, _) = c.ler(ate: 0.3)
            pacotes += try parser.alimentar(mais)
        }
        let st = try XCTUnwrap(pacotes.first { $0.tipo == 3 })
        let j = try XCTUnwrap(JSONSerialization.jsonObject(with: st.dados) as? [String: Any])
        XCTAssertEqual(j["lente"] as? String, "tele")
        XCTAssertEqual(j["foco"] as? String, "travado")
        XCTAssertEqual(j["exposicao"] as? String, "travada")
        XCTAssertEqual(j["branco"] as? String, "travado")
        XCTAssertEqual(j["bias"] as? Double, 0.7)
        XCTAssertEqual(j["w"] as? Int, 1920)
        XCTAssertEqual(j["h"] as? Int, 1080)
        XCTAssertEqual(j["fps_alvo"] as? Int, 30)
    }
}

// MARK: - Presets, exposição e foco manuais

final class PresetsETempoTests: XCTestCase {
    func testPresetsNovosNoHandshake() {
        func ped(_ json: String) -> PedidoHandshake? {
            if case .aceito(let p) = Handshake.interpretar(linha: Data(json.utf8)) { return p }
            return nil
        }
        XCTAssertEqual(ped(#"{"v":1,"preset":"2160p30"}"#)?.preset, .p2160_30)
        XCTAssertEqual(ped(#"{"v":1,"preset":"1080p120"}"#)?.preset, .p1080_120)
        XCTAssertEqual(ped(#"{"v":1,"preset":"2160p30"}"#)?.bitrate, 25_000_000)
        XCTAssertEqual(ped(#"{"v":1,"preset":"1080p120"}"#)?.bitrate, 20_000_000)
        XCTAssertEqual(ped(#"{"v":1,"preset":"1080p30"}"#)?.bitrate, 10_000_000)
        XCTAssertEqual(ped(#"{"v":1,"preset":"2160p30","bitrate":40000000}"#)?.bitrate, 40_000_000, "bitrate pedido vale")
        XCTAssertEqual(ped(#"{"v":1,"preset":"2160p30","bitrate":5}"#)?.bitrate, 25_000_000, "inválido cai no padrão do preset")
    }

    func testDimensoesDosPresets() {
        XCTAssertEqual([Preset.p2160_30.largura, Preset.p2160_30.altura, Preset.p2160_30.fps], [3840, 2160, 30])
        XCTAssertEqual([Preset.p1080_120.largura, Preset.p1080_120.altura, Preset.p1080_120.fps], [1920, 1080, 120])
        XCTAssertEqual([Preset.p720_60.largura, Preset.p720_60.altura, Preset.p720_60.fps], [1280, 720, 60])
    }

    func testQuedasDescemParaOMaisProximo() {
        XCTAssertEqual(Preset.p2160_30.quedas, [.p2160_30, .p1080_30, .p720_30])
        XCTAssertEqual(Preset.p1080_120.quedas, [.p1080_120, .p1080_60, .p1080_30, .p720_30])
        for p in Preset.allCases {
            XCTAssertEqual(p.quedas.first, p)
            XCTAssertEqual(p.quedas.last, .p720_30, "toda lista termina no mais básico")
        }
    }

    func testConfigVideoUsaBitrateDoPreset() {
        XCTAssertEqual(ConfigVideo(preset: .p2160_30).bitrate, 25_000_000)
        XCTAssertEqual(ConfigVideo().bitrate, 10_000_000)
        XCTAssertEqual(ConfigVideo(preset: .p2160_30, bitrate: 1_500_000).bitrate, 1_500_000)
    }

    func testObturadorTexto() {
        XCTAssertEqual(Obturador.texto(1.0 / 60), "1/60")
        XCTAssertEqual(Obturador.texto(1.0 / 8000), "1/8000")
        XCTAssertEqual(Obturador.texto(1.0 / 30.4), "1/30")
        XCTAssertEqual(Obturador.texto(2), "2 s")
        XCTAssertEqual(Obturador.texto(0), "-")
        XCTAssertEqual(Obturador.texto(.nan), "-")
    }

    func testAjustesManuaisPersistem() {
        let n = "teste-manual-\(UUID().uuidString)"
        guard let d = UserDefaults(suiteName: n) else { return XCTFail("sem defaults") }
        var a = AjustesCamera()
        a.focoModo = .manual; a.posicaoLente = 0.8; a.exposicaoModo = .manual; a.iso = 400; a.obturador = 1.0 / 125
        a.salvar(defaults: d)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d), a)
        d.removePersistentDomain(forName: n)
    }

    func testAjustesManuaisForaDaFaixaSaoLimitados() {
        let n = "teste-manual2-\(UUID().uuidString)"
        guard let d = UserDefaults(suiteName: n) else { return XCTFail("sem defaults") }
        d.set(Data(#"{"posicaoLente":7,"iso":99999999,"obturador":0,"focoModo":"x"}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.posicaoLente, 1)
        XCTAssertEqual(a.iso, 12800)
        XCTAssertGreaterThan(a.obturador, 0)
        XCTAssertEqual(a.focoModo, .auto)
        d.removePersistentDomain(forName: n)
    }
}

// MARK: - Tally e status novos

final class TallyEStatusTests: XCTestCase {
    final class Ultimo {
        private let trava = NSLock()
        private var _info = InfoServidor()
        func set(_ i: InfoServidor) { trava.lock(); _info = i; trava.unlock() }
        var info: InfoServidor { trava.lock(); defer { trava.unlock() }; return _info }
    }

    func subir(_ caixa: CaixaAjustes, ultimo: Ultimo) throws -> (Servidor, UInt16) {
        var op = Servidor.Opcoes()
        op.porta = 0; op.intervaloStatus = 0.2; op.bonjour = false; op.timeoutHandshake = 1
        let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "m", nome: "n", id: "i"),
                         ajustes: caixa, criarFonte: { FonteFalsa() })
        s.aoAtualizar = { ultimo.set($0) }
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { usleep(10_000) }
        return (s, try XCTUnwrap(s.portaAtual))
    }

    func esperar(_ seg: Double = 3, _ cond: () -> Bool) -> Bool {
        let fim = Date().addingTimeInterval(seg)
        while Date() < fim { if cond() { return true }; usleep(10_000) }
        return cond()
    }

    func comando(_ json: String) -> Data { Pacote(tipo: 4, tempo: 1, dados: Data(json.utf8)).serializar() }

    func testTallyLigaEDesliga() throws {
        let caixa = CaixaAjustes()
        let ultimo = Ultimo()
        let (s, porta) = try subir(caixa, ultimo: ultimo)
        defer { s.parar() }
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertNotNil(c.lerLinha())
        c.enviar(comando(#"{"cmd":"noar","valor":true}"#))
        XCTAssertTrue(esperar { ultimo.info.noAr }, "noar true deve acender o tally")
        XCTAssertTrue(caixa.noAr)
        c.enviar(comando(#"{"cmd":"noar","valor":false}"#))
        XCTAssertTrue(esperar { !ultimo.info.noAr })
        // valor que não é booleano e chave repetida são ignorados
        c.enviar(comando(#"{"cmd":"noar","valor":"sim"}"#))
        c.enviar(comando(#"{"cmd":"noar","cmd":"noar","valor":true}"#))
        usleep(300_000)
        XCTAssertFalse(ultimo.info.noAr)
        // ao sair o cliente o tally apaga
        c.enviar(comando(#"{"cmd":"noar","valor":true}"#))
        XCTAssertTrue(esperar { ultimo.info.noAr })
        close(c.fd)
        XCTAssertTrue(esperar { !ultimo.info.noAr && !caixa.noAr }, "tally apaga quando o cliente sai")
    }

    func testStatusTrazIsoObturadorEModos() throws {
        var a = AjustesCamera()
        a.focoModo = .manual; a.exposicaoModo = .manual
        let caixa = CaixaAjustes(a)
        caixa.leitura = LeituraCamera(iso: 160, obturadorSegundos: 1.0 / 60, posicaoLente: 0.4)
        let ultimo = Ultimo()
        let (s, porta) = try subir(caixa, ultimo: ultimo)
        defer { s.parar() }
        let c = try XCTUnwrap(ClienteCru(porta: porta))
        c.enviar(#"{"v":1}"# + "\n")
        let (d, _) = c.ler(ate: 3) { $0.contains(0x0A) }
        guard let idx = d.firstIndex(of: 0x0A) else { return XCTFail("sem resposta") }
        c.enviar(comando(#"{"cmd":"noar","valor":true}"#))
        var parser = ParserPacotes()
        var pacotes = try parser.alimentar(Data(d[(idx + 1)...]))
        let t0 = Date()
        while Date().timeIntervalSince(t0) < 3 {
            let (mais, _) = c.ler(ate: 0.3)
            pacotes += try parser.alimentar(mais)
            let j = pacotes.filter { $0.tipo == 3 }.compactMap { try? JSONSerialization.jsonObject(with: $0.dados) as? [String: Any] }
            if j.contains(where: { $0["noar"] as? Bool == true }) { break }
        }
        let status = pacotes.filter { $0.tipo == 3 }.compactMap { try? JSONSerialization.jsonObject(with: $0.dados) as? [String: Any] }
        guard let ultimoStatus = status.last else { return XCTFail("sem status") }
        XCTAssertEqual(ultimoStatus["iso"] as? Int, 160)
        XCTAssertEqual(ultimoStatus["obturador"] as? String, "1/60")
        XCTAssertEqual(ultimoStatus["foco"] as? String, "manual")
        XCTAssertEqual(ultimoStatus["exposicao"] as? String, "manual")
        XCTAssertNil(ultimoStatus["foco_modo"], "nomes antigos saíram do contrato")
        XCTAssertEqual(ultimoStatus["noar"] as? Bool, true)
    }
}

final class StatusCamposTests: XCTestCase {
    func testValoresDoContrato() {
        var a = AjustesCamera()
        let l = LeituraCamera(iso: 100, obturadorSegundos: 1.0 / 60, posicaoLente: 0.5)
        var c = StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)
        XCTAssertEqual(c["foco"] as? String, "auto")
        XCTAssertEqual(c["exposicao"] as? String, "auto")
        XCTAssertEqual(c["branco"] as? String, "auto")
        XCTAssertEqual(c["iso"] as? Int, 100)
        XCTAssertEqual(c["obturador"] as? String, "1/60")
        XCTAssertEqual(c["noar"] as? Bool, false)
        XCTAssertNil(c["w"], "sem formato ainda, sem w/h")
        a.focoModo = .auto
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "auto")
        a.focoTravado = true
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "travado")
        a.focoModo = .manual
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "manual")
        a.exposicaoTravada = true
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["exposicao"] as? String, "travada")
        a.exposicaoModo = .manual
        c = StatusCamera.campos(ajustes: a, leitura: l, noAr: true, dim: DimensoesVideo(largura: 3840, altura: 2160, fps: 30))
        XCTAssertEqual(c["exposicao"] as? String, "manual")
        XCTAssertEqual(c["noar"] as? Bool, true)
        XCTAssertEqual(c["w"] as? Int, 3840)
        XCTAssertEqual(c["fps_alvo"] as? Int, 30)
    }
}
