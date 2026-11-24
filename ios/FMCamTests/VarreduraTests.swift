import AVFoundation
import XCTest
@testable import FMCam

// Varredura 1: um teste por defeito achado, escrito para falhar antes da correção.

/// Motor que segura a troca de formato até o teste liberar (a câmera real leva de centenas de ms a 2 s).
final class MotorPresetLento: MotorDeCaptura {
    private let trava = NSLock()
    private var _pedidos: [Preset] = []
    private var pendentes: [(Result<DimensoesVideo, ErroFonte>) -> Void] = []
    var pedidos: [Preset] { trava.lock(); defer { trava.unlock() }; return _pedidos }

    func prepararParaTransmitir(_ config: ConfigVideo, cancelada: @escaping () -> Bool,
                                completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.success(DimensoesVideo(largura: 640, altura: 360, fps: 30)))
    }
    func definirConsumidor(dono: AnyObject, amostra: @escaping (CMSampleBuffer) -> Void,
                           aoFalha: @escaping (String) -> Void, aoReconfigurar: @escaping (DimensoesVideo) -> Void) {}
    func liberarConsumidor(dono: AnyObject) {}
    func mudarPreset(_ preset: Preset, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); _pedidos.append(preset); pendentes.append(completion); trava.unlock()
    }
    func terminarTodas() {
        trava.lock(); let p = pendentes; pendentes = []; trava.unlock()
        p.forEach { $0(.success(DimensoesVideo(largura: 640, altura: 360, fps: 30))) }
    }
}

/// Fonte que anota cada troca de qualidade pedida e nunca a termina (a troca fica "em andamento").
final class FonteTrocaLenta: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?
    private let trava = NSLock()
    private var _pedidos: [ConfigVideo] = []
    private var _iniciou = false
    private var inicioPendente: ((Result<DimensoesVideo, ErroFonte>) -> Void)?
    var segurarInicio = false
    var pedidos: [ConfigVideo] { trava.lock(); defer { trava.unlock() }; return _pedidos }
    var iniciou: Bool { trava.lock(); defer { trava.unlock() }; return _iniciou }

    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); _iniciou = true; trava.unlock()
        if segurarInicio { trava.lock(); inicioPendente = completion; trava.unlock(); return }
        completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
    }
    func liberarInicio() {
        trava.lock(); let p = inicioPendente; inicioPendente = nil; trava.unlock()
        p?(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
    }
    func parar() {}
    func pedirKeyframe() {}
    func reconfigurar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock(); _pedidos.append(config); trava.unlock()
    }
}

final class VarreduraServidorTests: XCTestCase {
    var servidor: Servidor?
    override func tearDown() { servidor?.parar(); servidor = nil }

    func subir(caixa: CaixaAjustes, fonte: FonteDeVideo) throws -> UInt16 {
        var op = Servidor.Opcoes()
        op.porta = 0
        op.bonjour = false
        op.timeoutPartidaCamera = 30
        let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "iPhone16,2", nome: "Cam", id: "ID-VARREDURA"),
                         ajustes: caixa) { fonte }
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return try XCTUnwrap(s.portaAtual)
    }

    private func salvar(_ caixa: CaixaAjustes, _ f: (inout AjustesCamera) -> Void) {
        var a = caixa.valor; f(&a); caixa.valor = a
    }

    /// Trocar a qualidade e voltar ao que era antes de a primeira troca terminar: a câmera tem que voltar também.
    /// Antes: a segunda troca era comparada com a qualidade JÁ aplicada (a antiga), parecia "nada mudou" e ficava só a primeira.
    func testVoltarAQualidadeAntigaComTrocaEmAndamentoPedeDeNovo() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteTrocaLenta()
        let porta = try subir(caixa: caixa, fonte: fonte)
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["ok"] as? Bool, true)

        salvar(caixa) { $0.preset = .p720_30 }
        servidor?.qualidadeMudou()
        XCTAssertTrue(c.esperar { fonte.pedidos.count == 1 })
        salvar(caixa) { $0.preset = .p1080_30 }
        servidor?.qualidadeMudou()
        XCTAssertTrue(c.esperar(1.5) { fonte.pedidos.count == 2 }, "a volta para 1080p30 sumiu: pedidos \(fonte.pedidos.map { $0.preset })")
        XCTAssertEqual(fonte.pedidos.map { $0.preset }, [.p720_30, .p1080_30])
    }

    /// Qualidade mudada no iPhone enquanto a câmera ainda estava ligando: ao ficar pronta a conexão tem que seguir o salvo.
    /// Antes: a mudança era descartada (fase iniciando) e a tela dizia uma coisa e o vídeo saía outra.
    func testQualidadeMudadaDuranteAPartidaValeQuandoACameraFicaPronta() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteTrocaLenta()
        fonte.segurarInicio = true
        let porta = try subir(caixa: caixa, fonte: fonte)
        let c = ClienteTeste(porta: porta)
        c.enviar(#"{"v":1}"# + "\n")
        XCTAssertTrue(c.esperar { fonte.iniciou })

        salvar(caixa) { $0.preset = .p720_60 }
        servidor?.qualidadeMudou()
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        fonte.liberarInicio()
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["ok"] as? Bool, true)
        XCTAssertTrue(c.esperar(1.5) { fonte.pedidos.count == 1 }, "a troca feita durante a partida foi perdida")
        XCTAssertEqual(fonte.pedidos.first?.preset, .p720_60)
    }
}

final class VarreduraCapturaTests: XCTestCase {
    /// Sem câmera conectada: troca de formato duas vezes em sequência rápida com a primeira ainda em andamento.
    func testFonteVoltaAoFormatoAntigoComATrocaAnteriorEmAndamento() {
        let motor = MotorPresetLento()
        let f = FonteCamera(motor: motor)
        var ok = false
        f.iniciar(ConfigVideo(preset: .p1080_30)) { if case .success = $0 { ok = true } }
        XCTAssertTrue(ok)
        var feitos = 0
        f.reconfigurar(ConfigVideo(preset: .p1080_60)) { _ in feitos += 1 }
        f.reconfigurar(ConfigVideo(preset: .p1080_30)) { _ in feitos += 1 }
        XCTAssertEqual(motor.pedidos, [.p1080_60, .p1080_30], "a volta ao 1080p30 foi tomada como 'sem mudança de formato'")
        motor.terminarTodas()
        XCTAssertEqual(feitos, 2)
        f.parar()
    }

    /// A câmera do app abre no formato salvo (antes abria sempre em 1080p30 e a 1a conexão em 4K reconfigurava dentro do
    /// limite de 2,5 s da partida).
    func testCameraAbreNoFormatoSalvo() {
        let c = CapturaCompartilhada()
        var a = AjustesCamera()
        a.preset = .p2160_30
        c.definirAjustesIniciais(a)
        Captura.filaSessao.sync {}
        XCTAssertEqual(c.preset, .p2160_30)
    }

    /// Erro de execução da câmera: uma religada a cada janela, não uma por vida do app.
    func testPoliticaDeReinicioVoltaATentarDepoisDeUmTempo() {
        var p = PoliticaDeReinicio()
        let t0 = Date()
        XCTAssertTrue(p.podeTentar(agora: t0))
        XCTAssertFalse(p.podeTentar(agora: t0.addingTimeInterval(1)), "falha em seguida: não fica em laço")
        XCTAssertTrue(p.podeTentar(agora: t0.addingTimeInterval(PoliticaDeReinicio.intervalo + 1)),
                      "horas depois um novo erro tem direito a uma religada")
        p.zerar()
        XCTAssertTrue(p.podeTentar(agora: t0.addingTimeInterval(PoliticaDeReinicio.intervalo + 2)))
    }
}

@MainActor
final class VarreduraTelaTests: XCTestCase {
    override func setUp() { UserDefaults.standard.removeObject(forKey: NomeCamera.chave) }
    override func tearDown() { UserDefaults.standard.removeObject(forKey: NomeCamera.chave) }

    /// Digitou o nome e tocou em outro item do menu: o nome tem que ficar salvo (antes sumia, a faixa de cima não mudava
    /// e o campo continuava com o texto digitado).
    func testNomeDigitadoFicaSalvoAoTrocarDeGrupo() {
        let m = ModeloApp()
        m.alternarGrupo(.ajustes)
        m.alternarPainel(.nome)
        m.campoNome = "Palco 7"
        m.alternarGrupo(.camera)
        XCTAssertEqual(m.nome, "Palco 7")
        XCTAssertEqual(m.campoNome, m.nome)
    }

    func testNomeDigitadoFicaSalvoAoLimparATela() {
        let m = ModeloApp()
        m.alternarGrupo(.ajustes)
        m.alternarPainel(.nome)
        m.campoNome = "Palco 8"
        m.alternarLimpo()
        XCTAssertEqual(m.nome, "Palco 8")
    }

    /// A marca "AE/AF travado" do toque longo some quando a trava é desligada (antes ficava na tela com a câmera em auto).
    func testMarcaDeTravadoSomeQuandoAsTravasSaem() {
        let m = ModeloApp()
        m.tocar(camada: CGPoint(x: 10, y: 10), dispositivo: CGPoint(x: 0.2, y: 0.2), travar: true)
        m.ajustes.focoTravado = true
        m.ajustes.exposicaoTravada = true
        XCTAssertEqual(m.indicadorFoco?.travado, true)
        m.alternarTrava(exposicao: true)
        XCTAssertFalse(m.ajustes.exposicaoTravada)
        XCTAssertNil(m.indicadorFoco, "a marca 'AE/AF travado' ficou na tela com a trava desligada")
    }

    /// O painel Qualidade mostra o que a câmera vai usar de verdade quando o formato salvo não existe na lente.
    func testPainelMostraOFormatoQueDeFatoSai() {
        let sup: Set<Preset> = [.p720_30, .p720_60, .p1080_30, .p1080_60]
        XCTAssertEqual(QualidadeEnvio.exibido(salvo: .p2160_30, suportados: sup), .p1080_30)
        XCTAssertEqual(QualidadeEnvio.exibido(salvo: .p1080_120, suportados: sup), .p1080_60)
        XCTAssertEqual(QualidadeEnvio.exibido(salvo: .p720_60, suportados: sup), .p720_60)
        XCTAssertEqual(QualidadeEnvio.exibido(salvo: .p2160_30, suportados: []), .p2160_30, "sem nada na lista, mostra o salvo")
    }
}

final class PrimeiroPlanoRepetidoTests: XCTestCase {
    func testSegundaChamadaComACameraDePeNaoMexeNela() {
        XCTAssertTrue(ControleDePartida.deveLigarPreview(jaEmPrimeiroPlano: false, sessaoRodando: false, configurada: false))
        XCTAssertFalse(ControleDePartida.deveLigarPreview(jaEmPrimeiroPlano: true, sessaoRodando: true, configurada: true))
        XCTAssertTrue(ControleDePartida.deveLigarPreview(jaEmPrimeiroPlano: true, sessaoRodando: false, configurada: true), "voltou do segundo plano: liga")
        XCTAssertTrue(ControleDePartida.deveLigarPreview(jaEmPrimeiroPlano: true, sessaoRodando: true, configurada: false))
    }
}
