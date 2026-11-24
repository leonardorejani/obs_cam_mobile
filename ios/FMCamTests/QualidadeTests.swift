import Network
import XCTest
@testable import FMCam

/// Fonte falsa que aceita troca de qualidade ao vivo e guarda o que foi pedido.
final class FonteReconfiguravel: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    private let trava = NSLock()
    private var _iniciou: ConfigVideo?
    private var _trocas: [ConfigVideo] = []
    private var _parada = false
    var iniciou: ConfigVideo? { trava.lock(); defer { trava.unlock() }; return _iniciou }
    var trocas: [ConfigVideo] { trava.lock(); defer { trava.unlock() }; return _trocas }
    var parada: Bool { trava.lock(); defer { trava.unlock() }; return _parada }

    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); _iniciou = config; trava.unlock()
        completion(.success(DimensoesVideo(largura: config.preset.largura, altura: config.preset.altura, fps: config.preset.fps)))
        aoEvento?(.config(tempo: 1000, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
        aoEvento?(.quadro(tempo: 1000, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
    }

    func reconfigurar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); _trocas.append(config); trava.unlock()
        completion(.success(DimensoesVideo(largura: config.preset.largura, altura: config.preset.altura, fps: config.preset.fps)))
        // formato novo manda tipo 1 e quadro-chave
        aoEvento?(.config(tempo: 2000, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
        aoEvento?(.quadro(tempo: 2000, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
    }

    func parar() { trava.lock(); _parada = true; trava.unlock() }
    func pedirKeyframe() {}
}

final class QualidadeRegrasTests: XCTestCase {
    private func pedido(_ json: String) throws -> PedidoHandshake {
        guard case .aceito(let p) = Handshake.interpretar(linha: Data(json.utf8)) else { throw XCTSkip("handshake recusado: \(json)") }
        return p
    }

    func testPadraoSalvo() {
        let a = AjustesCamera()
        XCTAssertEqual(a.preset, .p1080_30)
        XCTAssertEqual(a.bitrate, 10_000_000)
    }

    func testPersistenciaDaQualidade() throws {
        let d = try XCTUnwrap(UserDefaults(suiteName: "fmcam.teste.\(UUID().uuidString)"))
        var a = AjustesCamera()
        a.preset = .p1080_60
        a.bitrate = 20_000_000
        a.salvar(defaults: d)
        let b = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(b.preset, .p1080_60)
        XCTAssertEqual(b.bitrate, 20_000_000)
    }

    func testAjustesAntigosSemQualidadeCaemNoPadrao() throws {
        let d = try XCTUnwrap(UserDefaults(suiteName: "fmcam.teste.\(UUID().uuidString)"))
        d.set(Data(#"{"zoom":2}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.preset, .p1080_30)
        XCTAssertEqual(a.bitrate, 10_000_000)
        XCTAssertEqual(a.zoom, 2)
    }

    func testHandshakeSemPresetNemBitrateUsaOSalvo() throws {
        let p = try pedido(#"{"v":1}"#)
        XCTAssertFalse(p.presetInformado)
        XCTAssertFalse(p.bitrateInformado)
        var salva = AjustesCamera()
        salva.preset = .p720_60
        salva.bitrate = 6_000_000
        let q = QualidadeEnvio.resolver(pedido: p, salva: salva)
        XCTAssertEqual(q, QualidadeEnvio(preset: .p720_60, bitrate: 6_000_000, presetObs: false, bitrateObs: false))
    }

    func testHandshakeComPresetEBitrateValeODoOBS() throws {
        let p = try pedido(#"{"v":1,"preset":"2160p30","bitrate":30000000}"#)
        XCTAssertTrue(p.presetInformado && p.bitrateInformado)
        var salva = AjustesCamera()
        salva.preset = .p720_30
        let q = QualidadeEnvio.resolver(pedido: p, salva: salva)
        XCTAssertEqual(q, QualidadeEnvio(preset: .p2160_30, bitrate: 30_000_000, presetObs: true, bitrateObs: true))
    }

    func testSoPresetTravaSoOPreset() throws {
        let p = try pedido(#"{"v":1,"preset":"1080p120"}"#)
        var salva = AjustesCamera()
        salva.bitrate = 15_000_000
        let q = QualidadeEnvio.resolver(pedido: p, salva: salva)
        XCTAssertEqual(q, QualidadeEnvio(preset: .p1080_120, bitrate: 15_000_000, presetObs: true, bitrateObs: false))
        XCTAssertTrue(q.escolhidaNoOBS)
    }

    func testSoBitrateTravaSoOBitrateEPresetFicaLivre() throws {
        // caso real: a extensão mandou bitrate 20000000 sem preset
        let p = try pedido(#"{"v":1,"bitrate":20000000}"#)
        var salva = AjustesCamera()
        salva.preset = .p1080_60
        let q = QualidadeEnvio.resolver(pedido: p, salva: salva)
        XCTAssertEqual(q, QualidadeEnvio(preset: .p1080_60, bitrate: 20_000_000, presetObs: false, bitrateObs: true))
        // o iPhone ainda troca o formato no meio da conexão, o bitrate fica o do OBS
        var depois = salva
        depois.preset = .p720_30
        depois.bitrate = 6_000_000
        let nova = q.comSalva(depois)
        XCTAssertEqual(nova.preset, .p720_30)
        XCTAssertEqual(nova.bitrate, 20_000_000)
    }

    func testSoPresetDoOBSBitrateSegueOSalvoAoVivo() throws {
        let q = QualidadeEnvio(preset: .p1080_30, bitrate: 10_000_000, presetObs: true, bitrateObs: false)
        var salva = AjustesCamera()
        salva.preset = .p720_30
        salva.bitrate = 30_000_000
        let nova = q.comSalva(salva)
        XCTAssertEqual(nova.preset, .p1080_30)
        XCTAssertEqual(nova.bitrate, 30_000_000)
    }

    func testAvisosPorCampo() {
        XCTAssertNil(QualidadeEnvio.aviso(presetObs: false, bitrateObs: false))
        XCTAssertEqual(QualidadeEnvio.aviso(presetObs: false, bitrateObs: true), "Bitrate escolhido no OBS.")
        XCTAssertEqual(QualidadeEnvio.aviso(presetObs: true, bitrateObs: false), "Formato escolhido no OBS.")
        XCTAssertEqual(QualidadeEnvio.aviso(presetObs: true, bitrateObs: true),
                       "Formato e bitrate escolhidos no OBS. Para mudar aqui, deixe o OBS em Definida no iPhone.")
    }

    func testBitrateForaDaFaixaNaoContaComoInformado() throws {
        let p = try pedido(#"{"v":1,"bitrate":5}"#)
        XCTAssertFalse(p.bitrateInformado)
    }

    func testRotulos() {
        XCTAssertEqual(QualidadeEnvio.rotulo(bitrate: 10_000_000), "10 Mbps")
        XCTAssertEqual(QualidadeEnvio.rotulo(bitrate: 6_000_000), "6 Mbps")
        XCTAssertEqual(QualidadeEnvio.rotulo(bitrate: 6_500_000), "6,5 Mbps")
        XCTAssertEqual(QualidadeEnvio.formatos.map(\.rawValue), ["720p30", "720p60", "1080p30", "1080p60", "1080p120", "2160p30"])
        XCTAssertEqual(QualidadeEnvio.bitrates.count, 5)
        XCTAssertEqual(QualidadeEnvio.preset(de: DimensoesVideo(largura: 1920, altura: 1080, fps: 60)), .p1080_60)
        XCTAssertNil(QualidadeEnvio.preset(de: nil))
    }
}

final class ResolucaoQuadrosTests: XCTestCase {
    private let todos = Set(Preset.allCases)

    func testCombinacoesValidas() {
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r720, fps: 30), .p720_30)
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r720, fps: 60), .p720_60)
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r1080, fps: 30), .p1080_30)
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r1080, fps: 60), .p1080_60)
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r1080, fps: 120), .p1080_120)
        XCTAssertEqual(QualidadeEnvio.preset(resolucao: .r4k, fps: 30), .p2160_30)
    }

    func testCombinacoesQueNaoExistem() {
        XCTAssertNil(QualidadeEnvio.preset(resolucao: .r720, fps: 120))
        XCTAssertNil(QualidadeEnvio.preset(resolucao: .r4k, fps: 60))
        XCTAssertNil(QualidadeEnvio.preset(resolucao: .r4k, fps: 120))
        XCTAssertFalse(QualidadeEnvio.disponivel(.r4k, 60, suportados: todos))
    }

    func testTodoPresetTemUmaCombinacao() {
        for p in Preset.allCases {
            XCTAssertEqual(QualidadeEnvio.preset(resolucao: Resolucao.de(p), fps: p.fps), p)
        }
    }

    func testLenteSemSuporteDesabilita() {
        let sup: Set<Preset> = [.p720_30, .p1080_30, .p1080_60]
        XCTAssertFalse(QualidadeEnvio.disponivel(.r1080, 120, suportados: sup))
        XCTAssertFalse(QualidadeEnvio.disponivel(.r4k, 30, suportados: sup))
        XCTAssertTrue(QualidadeEnvio.disponivel(.r1080, 60, suportados: sup))
    }

    func testQuedaDeFpsAoTrocarResolucao() {
        // 1080p120 para 720p: 720p120 não existe, cai para 60
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r720, fpsAtual: 120, suportados: todos), .p720_60)
        // 1080p60 para 4K: cai para 30
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r4k, fpsAtual: 60, suportados: todos), .p2160_30)
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r4k, fpsAtual: 120, suportados: todos), .p2160_30)
        // fps existe: mantém
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r1080, fpsAtual: 60, suportados: todos), .p1080_60)
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r720, fpsAtual: 30, suportados: todos), .p720_30)
    }

    func testQuedaRespeitaALente() {
        let sup: Set<Preset> = [.p720_30, .p1080_30, .p1080_60]
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r720, fpsAtual: 60, suportados: sup), .p720_30)
        XCTAssertNil(QualidadeEnvio.trocar(resolucao: .r4k, fpsAtual: 30, suportados: sup), "resolução sem nenhum formato na lente")
    }

    func testSubirDeFpsQuandoSoHaAcima() {
        let sup: Set<Preset> = [.p1080_60]
        XCTAssertEqual(QualidadeEnvio.trocar(resolucao: .r1080, fpsAtual: 30, suportados: sup), .p1080_60)
    }

    func testRotulosDoBitrate() {
        XCTAssertEqual(QualidadeEnvio.numero(bitrate: 10_000_000), "10")
        XCTAssertEqual(QualidadeEnvio.bitrates.map { QualidadeEnvio.numero(bitrate: $0) }, ["6", "10", "15", "20", "30"])
        XCTAssertEqual(Resolucao.allCases.map(\.rawValue), ["720p", "1080p", "4K"])
        XCTAssertEqual(QualidadeEnvio.quadros, [30, 60, 120])
    }
}

final class QualidadeServidorTests: XCTestCase {
    let identidade = IdentidadeAparelho(modelo: "iPhone14,5", nome: "Camera 1", id: "UUID-TESTE")
    var servidor: Servidor?
    override func tearDown() { servidor?.parar(); servidor = nil }

    private func subir(_ fonte: FonteReconfiguravel, _ caixa: CaixaAjustes) throws -> UInt16 {
        var op = Servidor.Opcoes()
        op.porta = 0
        op.bonjour = false
        op.intervaloStatus = 0.2
        let s = Servidor(opcoes: op, identidade: identidade, ajustes: caixa, criarFonte: { fonte })
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return try XCTUnwrap(s.portaAtual)
    }

    private func statusAtual(_ c: ClienteTeste, _ cond: @escaping ([String: Any]) -> Bool) -> [String: Any]? {
        var achado: [String: Any]?
        _ = c.esperarPacotes(0) { pacotes in
            let js = pacotes.filter { $0.tipo == 3 }.compactMap { try? JSONSerialization.jsonObject(with: $0.dados) as? [String: Any] }
            achado = js.last(where: cond)
            return achado != nil
        }
        return achado
    }

    func testSemPresetNoHandshakeUsaSalvaEReconfiguraSemDerrubar() throws {
        var a = AjustesCamera()
        a.preset = .p720_60
        a.bitrate = 6_000_000
        let caixa = CaixaAjustes(a)
        let fonte = FonteReconfiguravel()
        let porta = try subir(fonte, caixa)
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1,"cliente":"teste"}"# + "\n")
        let resp = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(resp["w"] as? Int, 1280)
        XCTAssertEqual(fonte.iniciou?.preset, .p720_60)
        XCTAssertEqual(fonte.iniciou?.bitrate, 6_000_000)

        let s1 = try XCTUnwrap(statusAtual(c) { $0["bitrate"] != nil })
        XCTAssertEqual(s1["bitrate"] as? Int, 6_000_000)
        XCTAssertEqual(s1["qualidade_obs"] as? Bool, false)
        XCTAssertEqual(s1["w"] as? Int, 1280)

        // muda no iPhone durante a conexão
        var nova = caixa.valor
        nova.preset = .p1080_60
        nova.bitrate = 20_000_000
        caixa.valor = nova
        servidor?.qualidadeMudou()
        XCTAssertTrue(c.esperar { fonte.trocas.count == 1 }, "reconfigurou a fonte")
        XCTAssertEqual(fonte.trocas.first?.preset, .p1080_60)
        XCTAssertEqual(fonte.trocas.first?.bitrate, 20_000_000)
        let s2 = try XCTUnwrap(statusAtual(c) { $0["w"] as? Int == 1920 && $0["bitrate"] as? Int == 20_000_000 })
        XCTAssertEqual(s2["h"] as? Int, 1080)
        XCTAssertEqual(s2["fps_alvo"] as? Int, 60)
        XCTAssertFalse(c.estaFechado, "a conexão continua aberta")
        XCTAssertFalse(fonte.parada)
        // dois tipos 1: o do início e o da troca
        XCTAssertGreaterThanOrEqual(c.pacotes.filter { $0.tipo == 1 }.count, 2)
    }

    func testSoBitrateDoOBSTravaSoOBitrate() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteReconfiguravel()
        let porta = try subir(fonte, caixa)
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1,"bitrate":20000000}"# + "\n")
        XCTAssertNotNil(c.lerLinha())
        XCTAssertEqual(fonte.iniciou?.bitrate, 20_000_000)
        XCTAssertEqual(fonte.iniciou?.preset, .p1080_30)
        let s1 = try XCTUnwrap(statusAtual(c) { $0["bitrate_obs"] != nil })
        XCTAssertEqual(s1["bitrate_obs"] as? Bool, true)
        XCTAssertEqual(s1["preset_obs"] as? Bool, false)
        XCTAssertEqual(s1["qualidade_obs"] as? Bool, true)
        XCTAssertEqual(s1["bitrate"] as? Int, 20_000_000)

        // formato muda no iPhone no meio da conexão, bitrate não
        var nova = caixa.valor
        nova.preset = .p720_60
        nova.bitrate = 6_000_000
        caixa.valor = nova
        servidor?.qualidadeMudou()
        XCTAssertTrue(c.esperar { fonte.trocas.count == 1 })
        XCTAssertEqual(fonte.trocas.first?.preset, .p720_60)
        XCTAssertEqual(fonte.trocas.first?.bitrate, 20_000_000)
        XCTAssertFalse(c.estaFechado)
    }

    /// Varredura 2: sem OBS conectado, mudar a qualidade no iPhone não chegava à câmera (o preview seguia no formato velho e o
    /// "Formato" do topo dizia outro valor que o painel). Agora o servidor entrega o formato salvo para o preview.
    func testQualidadeSemConexaoVaiParaACamera() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteReconfiguravel()
        _ = try subir(fonte, caixa)
        let recebidos = CaixaPresets()
        servidor?.aoQualidadeSemConexao = { recebidos.add($0) }
        var nova = caixa.valor
        nova.preset = .p2160_30
        caixa.valor = nova
        servidor?.qualidadeMudou()
        let fim = Date().addingTimeInterval(2)
        while recebidos.lista.isEmpty && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        XCTAssertEqual(recebidos.lista, [.p2160_30])
    }

    func testQualidadeComConexaoNaoMexeNoPreview() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteReconfiguravel()
        let porta = try subir(fonte, caixa)
        let recebidos = CaixaPresets()
        servidor?.aoQualidadeSemConexao = { recebidos.add($0) }
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertNotNil(c.lerLinha())
        var nova = caixa.valor
        nova.preset = .p720_30
        caixa.valor = nova
        servidor?.qualidadeMudou()
        XCTAssertTrue(c.esperar { fonte.trocas.count == 1 }, "com conexão a sessão troca")
        XCTAssertTrue(recebidos.lista.isEmpty, "o preview não é mexido por fora da sessão")
    }

    func testPresetEBitrateDoOBSNaoMudamPeloIPhone() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteReconfiguravel()
        let porta = try subir(fonte, caixa)
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1,"preset":"1080p30","bitrate":15000000}"# + "\n")
        XCTAssertNotNil(c.lerLinha())
        let s1 = try XCTUnwrap(statusAtual(c) { $0["qualidade_obs"] != nil })
        XCTAssertEqual(s1["preset_obs"] as? Bool, true)
        XCTAssertEqual(s1["bitrate_obs"] as? Bool, true)
        var nova = caixa.valor
        nova.preset = .p720_30
        nova.bitrate = 6_000_000
        caixa.valor = nova
        servidor?.qualidadeMudou()
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        XCTAssertTrue(fonte.trocas.isEmpty, "o OBS fixou os dois, o iPhone não troca")
        XCTAssertFalse(c.estaFechado)
    }
}

private final class CaixaPresets {
    private let trava = NSLock()
    private var _lista: [Preset] = []
    func add(_ p: Preset) { trava.lock(); _lista.append(p); trava.unlock() }
    var lista: [Preset] { trava.lock(); defer { trava.unlock() }; return _lista }
}
