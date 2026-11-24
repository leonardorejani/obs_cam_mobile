import CoreMedia
import CoreVideo
import Network
import SwiftUI
import UIKit
import XCTest
@testable import FMCam

/// Mapa orientação física para ângulo do vídeo, tamanho girado e debounce.
final class OrientacaoLogicaTests: XCTestCase {
    func testPosicaoFisicaParaAnguloDoVideo() {
        // landscapeLeft (botão início à direita) é a posição nativa do sensor, e o inverso do UIInterfaceOrientation
        XCTAssertEqual(OrientacaoVideo.angulo(de: .landscapeLeft), 0)
        XCTAssertEqual(OrientacaoVideo.angulo(de: .portrait), 90)
        XCTAssertEqual(OrientacaoVideo.angulo(de: .landscapeRight), 180)
        XCTAssertEqual(OrientacaoVideo.angulo(de: .portraitUpsideDown), 270)
    }

    func testDeitadoEDesconhecidoNaoTemAngulo() {
        XCTAssertNil(OrientacaoVideo.angulo(de: .faceUp))
        XCTAssertNil(OrientacaoVideo.angulo(de: .faceDown))
        XCTAssertNil(OrientacaoVideo.angulo(de: .unknown))
    }

    func testAnguloDoSistemaVaiParaMultiploDe90() {
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 0), 0)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 90), 90)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 181), 180)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 269.9), 270)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 360), 0)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: -90), 270)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: 46), 90)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: .nan), 0)
        XCTAssertEqual(OrientacaoVideo.angulo(graus: .infinity), 0)
    }

    func testEmPeSoNos90E270() {
        XCTAssertEqual(OrientacaoVideo.angulos.filter(OrientacaoVideo.emPe), [90, 270])
    }

    func testTamanhoDoVideoEmPeEhOTransposto() {
        let p1080 = DimensoesVideo(largura: 1920, altura: 1080, fps: 30)
        XCTAssertEqual(OrientacaoVideo.dimensoes(p1080, angulo: 0), p1080)
        XCTAssertEqual(OrientacaoVideo.dimensoes(p1080, angulo: 180), p1080, "180 só vira de cabeça para baixo")
        XCTAssertEqual(OrientacaoVideo.dimensoes(p1080, angulo: 90), DimensoesVideo(largura: 1080, altura: 1920, fps: 30))
        XCTAssertEqual(OrientacaoVideo.dimensoes(p1080, angulo: 270), DimensoesVideo(largura: 1080, altura: 1920, fps: 30))
        // as outras resoluções transpostas
        XCTAssertEqual(OrientacaoVideo.dimensoes(DimensoesVideo(largura: 1280, altura: 720, fps: 60), angulo: 90),
                       DimensoesVideo(largura: 720, altura: 1280, fps: 60))
        XCTAssertEqual(OrientacaoVideo.dimensoes(DimensoesVideo(largura: 3840, altura: 2160, fps: 30), angulo: 90),
                       DimensoesVideo(largura: 2160, altura: 3840, fps: 30))
    }

    func testReorientarSoTrocaQuandoCruzaPaisagemEEmPe() {
        let pais = DimensoesVideo(largura: 1920, altura: 1080, fps: 30)
        let pe = DimensoesVideo(largura: 1080, altura: 1920, fps: 30)
        XCTAssertEqual(OrientacaoVideo.reorientar(pais, de: 0, para: 180), pais)
        XCTAssertEqual(OrientacaoVideo.reorientar(pais, de: 180, para: 90), pe)
        XCTAssertEqual(OrientacaoVideo.reorientar(pe, de: 90, para: 270), pe)
        XCTAssertEqual(OrientacaoVideo.reorientar(pe, de: 270, para: 0), pais)
    }

    func testFormatoEQualidadeNaoSeConfundemEmPe() {
        let pe = DimensoesVideo(largura: 1080, altura: 1920, fps: 60)
        XCTAssertEqual(Rotulos.formato(pe), "1080p · 60", "o rótulo do formato é o mesmo em pé")
        XCTAssertEqual(QualidadeEnvio.preset(de: pe), .p1080_60)
        XCTAssertEqual(QualidadeEnvio.preset(de: DimensoesVideo(largura: 1920, altura: 1080, fps: 60)), .p1080_60)
    }

    func testOrientacaoDeVideoDoIOS16() {
        XCTAssertEqual(OrientacaoVideo.orientacaoDeVideo(0), .landscapeRight)
        XCTAssertEqual(OrientacaoVideo.orientacaoDeVideo(90), .portrait)
        XCTAssertEqual(OrientacaoVideo.orientacaoDeVideo(180), .landscapeLeft)
        XCTAssertEqual(OrientacaoVideo.orientacaoDeVideo(270), .portraitUpsideDown)
    }

    // MARK: debounce

    private let t0 = Date(timeIntervalSince1970: 1_000_000)

    func testTrocaSoVale_depoisDeMeioSegundoParada() {
        var d = DebounceOrientacao(inicial: 0)
        d.entrada(90, agora: t0)
        XCTAssertNil(d.pronto(agora: t0))
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(0.49)))
        XCTAssertEqual(d.pronto(agora: t0.addingTimeInterval(0.5)), 90)
        XCTAssertEqual(d.aplicado, 90)
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(2)), "uma troca devolve uma vez só")
    }

    func testVoltarParaOAplicadoCancelaATroca() {
        var d = DebounceOrientacao(inicial: 0)
        d.entrada(90, agora: t0)
        d.entrada(0, agora: t0.addingTimeInterval(0.2))
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(1)))
        XCTAssertEqual(d.aplicado, 0)
        XCTAssertFalse(d.pendente)
    }

    func testPassarPorPosicaoIntermediariaReiniciaAJanela() {
        var d = DebounceOrientacao(inicial: 0)
        d.entrada(90, agora: t0)
        d.entrada(180, agora: t0.addingTimeInterval(0.4))  // girou mais: conta de novo
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(0.8)))
        XCTAssertEqual(d.pronto(agora: t0.addingTimeInterval(0.9)), 180, "vale o último lado, nunca o do meio")
    }

    func testRepetirOMesmoCandidatoNaoReiniciaAJanela() {
        var d = DebounceOrientacao(inicial: 0)
        d.entrada(90, agora: t0)
        d.entrada(90, agora: t0.addingTimeInterval(0.3))
        d.entrada(90, agora: t0.addingTimeInterval(0.45))
        XCTAssertEqual(d.pronto(agora: t0.addingTimeInterval(0.5)), 90)
    }

    func testDeitadoEIgnorado() {
        var d = DebounceOrientacao(inicial: 0)
        d.entrada(nil, agora: t0)  // faceUp, faceDown ou desconhecido
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(5)))
        XCTAssertEqual(d.aplicado, 0)
        // e não atrapalha uma troca já esperando
        d.entrada(90, agora: t0)
        d.entrada(nil, agora: t0.addingTimeInterval(0.3))
        XCTAssertEqual(d.pronto(agora: t0.addingTimeInterval(0.5)), 90)
    }

    func testSemTrocaNaoHaNadaPendente() {
        var d = DebounceOrientacao(inicial: 180)
        d.entrada(180, agora: t0)
        XCTAssertFalse(d.pendente)
        XCTAssertNil(d.pronto(agora: t0.addingTimeInterval(9)))
    }
}

/// Nível horizontal em pé.
final class NivelEmPeTests: XCTestCase {
    private func rad(_ g: Double) -> Double { g * .pi / 180 }

    func testPaisagemContinuaIgual() {
        for (gx, gy) in [(-1.0, 0.0), (-cos(rad(10)), sin(rad(10))), (cos(rad(10)), sin(rad(10))), (1.0, 0.0)] {
            XCTAssertEqual(Nivel.anguloEmQualquerPosicao(gx: gx, gy: gy), Nivel.angulo(gx: gx, gy: gy))
        }
    }

    func testEmPeNiveladoEZero() {
        XCTAssertEqual(Nivel.anguloEmQualquerPosicao(gx: 0, gy: -1) ?? 99, 0, accuracy: 0.001)
        XCTAssertEqual(Nivel.anguloEmQualquerPosicao(gx: 0, gy: 1) ?? 99, 0, accuracy: 0.001, "de cabeça para baixo")
    }

    func testEmPeInclinadoDaSinalOpostoParaCadaLado() {
        let a = Nivel.anguloEmQualquerPosicao(gx: sin(rad(10)), gy: -cos(rad(10)))
        let b = Nivel.anguloEmQualquerPosicao(gx: -sin(rad(10)), gy: -cos(rad(10)))
        XCTAssertEqual(abs(a ?? 0), 10, accuracy: 0.001)
        XCTAssertEqual(abs(b ?? 0), 10, accuracy: 0.001)
        XCTAssertEqual((a ?? 0) + (b ?? 0), 0, accuracy: 0.001)
        XCTAssertNotEqual(a, b)
    }

    func testDeitadoNaMesaNaoTemNivel() {
        XCTAssertNil(Nivel.anguloEmQualquerPosicao(gx: 0.1, gy: 0.2))
        XCTAssertNil(Nivel.anguloEmQualquerPosicao(gx: 0, gy: 0))
    }

    func testEmPeSemLadoClaroNaoChuta() {
        XCTAssertNil(Nivel.anguloEmQualquerPosicao(gx: 0.3, gy: -0.45))
    }
}

/// Giro com o OBS conectado: tamanho novo, tipo 1 novo e quadro-chave, sem derrubar a fonte.
final class GiroDaFonteTests: XCTestCase {
    func iniciar(_ f: FonteCamera) -> Result<DimensoesVideo, ErroFonte>? {
        var r: Result<DimensoesVideo, ErroFonte>?
        f.iniciar(ConfigVideo()) { r = $0 }
        return r
    }

    func testGiroParaEmPeRecriaOCodificadorEAvisaOTamanho() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = FonteCameraTests.Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        for i in 0..<3 { motor.entregar(amostraSintetica(i)) }
        f.esvaziarCodificador()
        let emPe = OrientacaoVideo.dimensoes(motor.dim, angulo: 90)
        XCTAssertEqual(emPe, DimensoesVideo(largura: 360, altura: 640, fps: 30))
        motor.reconfigurar(emPe)
        for i in 3..<6 { motor.entregar(amostraSintetica(i, w: 360, h: 640)) }
        f.parar()
        XCTAssertEqual(col.configs, 2, "tipo 1 de novo com o SPS do tamanho em pé")
        XCTAssertTrue(col.eventos.contains { if case .dimensao(let d) = $0 { return d == emPe }; return false }, "avisa o tamanho novo")
        let configs = col.eventos.compactMap { e -> Data? in if case .config(_, let d) = e { return d }; return nil }
        XCTAssertEqual(configs.count, 2)
        if configs.count == 2 { XCTAssertNotEqual(configs[0], configs[1], "o SPS de em pé não é o de paisagem") }
        XCTAssertEqual(col.quadros.count, 6)
        guard col.quadros.count == 6 else { return XCTFail("quadros: \(col.quadros.count)") }
        XCTAssertTrue(col.quadros[3].0, "o primeiro quadro em pé é chave")
    }

    func testGiroDe180NaoMudaOTamanhoSoMandaConfigEChave() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = FonteCameraTests.Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        for i in 0..<3 { motor.entregar(amostraSintetica(i)) }
        f.esvaziarCodificador()
        motor.reconfigurar(OrientacaoVideo.dimensoes(motor.dim, angulo: 180))
        motor.entregar(amostraSintetica(3))
        f.parar()
        XCTAssertEqual(col.configs, 2)
        XCTAssertFalse(col.eventos.contains { if case .dimensao = $0 { return true }; return false }, "o tamanho não mudou")
        guard col.quadros.count == 4 else { return XCTFail("quadros: \(col.quadros.count)") }
        XCTAssertTrue(col.quadros[3].0)
    }

    func testQuadroDoTamanhoAntigoDepoisDoGiroNaoEntraNoCodificadorNovo() {
        let motor = MotorFalso()
        let f = FonteCamera(motor: motor)
        let col = FonteCameraTests.Coletor()
        f.aoEvento = col.adicionar
        _ = iniciar(f)
        motor.reconfigurar(OrientacaoVideo.dimensoes(motor.dim, angulo: 90))
        // um quadro que a câmera ainda tinha na fila no tamanho de antes do giro
        motor.entregar(amostraSintetica(0, w: 640, h: 360))
        motor.entregar(amostraSintetica(1, w: 360, h: 640))
        f.esvaziarCodificador()
        f.parar()
        XCTAssertEqual(col.quadros.count, 1, "só o quadro em pé foi codificado")
        XCTAssertTrue(col.quadros.first?.0 ?? false, "e ele é chave: o descartado não gasta o pedido de chave")
    }

    func testCodificadorEmPeEmitePrimeiroQuadroChave() throws {
        let cod = try Codificador(largura: 360, altura: 640, fps: 30, bitrate: 2_000_000)
        let col = FonteCameraTests.Coletor()
        cod.aoEvento = col.adicionar
        for i in 0..<3 {
            var pb: CVPixelBuffer?
            CVPixelBufferCreate(nil, 360, 640, kCVPixelFormatType_32BGRA, nil, &pb)
            cod.codificar(pixelBuffer: try XCTUnwrap(pb), pts: CMTime(value: Int64(i), timescale: 30))
        }
        cod.finalizar()
        XCTAssertEqual(col.configs, 1)
        XCTAssertEqual(col.quadros.count, 3)
    }
}

/// O status do iPhone passa a dizer o tamanho novo depois do giro.
final class GiroNoServidorTests: XCTestCase {
    final class FonteQueGira: FonteDeVideo {
        var aoEvento: ((EventoVideo) -> Void)?
        func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
            completion(.success(DimensoesVideo(largura: 1920, altura: 1080, fps: 30)))
            aoEvento?(.config(tempo: 1000, dados: Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE])))
            aoEvento?(.quadro(tempo: 1000, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
        }
        func girarParaEmPe() {
            aoEvento?(.dimensao(DimensoesVideo(largura: 1080, altura: 1920, fps: 30)))
            aoEvento?(.config(tempo: 2000, dados: Data([0, 0, 0, 1, 0x67, 0x43, 0, 0, 0, 1, 0x68, 0xCF])))
            aoEvento?(.quadro(tempo: 2000, chave: true, dados: Data([0, 0, 0, 1, 0x65])))
        }
        func parar() {}
        func pedirKeyframe() {}
    }

    var servidor: Servidor?
    override func tearDown() { servidor?.parar(); servidor = nil }

    func testStatusSegueOTamanhoEmPeSemDerrubarAConexao() throws {
        let fonte = FonteQueGira()
        var op = Servidor.Opcoes()
        op.porta = 0
        op.bonjour = false
        op.intervaloStatus = 0.2
        let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "iPhone16,2", nome: "Cam", id: "ID-GIRO"), criarFonte: { fonte })
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        let c = ClienteTeste(porta: try XCTUnwrap(s.portaAtual))
        c.enviar(#"{"v":1,"cliente":"teste"}"# + "\n")
        let resp = try XCTUnwrap(c.lerLinha())
        XCTAssertEqual(resp["w"] as? Int, 1920)
        XCTAssertEqual(resp["h"] as? Int, 1080)
        func status(_ cond: @escaping ([String: Any]) -> Bool) -> [String: Any]? {
            var achado: [String: Any]?
            _ = c.esperarPacotes(0) { pacotes in
                let js = pacotes.filter { $0.tipo == 3 }.compactMap { try? JSONSerialization.jsonObject(with: $0.dados) as? [String: Any] }
                achado = js.last(where: cond)
                return achado != nil
            }
            return achado
        }
        XCTAssertNotNil(status { $0["w"] as? Int == 1920 })
        fonte.girarParaEmPe()
        let depois = try XCTUnwrap(status { $0["w"] as? Int == 1080 && $0["h"] as? Int == 1920 })
        XCTAssertEqual(depois["fps_alvo"] as? Int, 30)
        XCTAssertFalse(c.estaFechado, "girar não derruba a conexão")
        XCTAssertEqual(c.pacotes.filter { $0.tipo == 1 }.count, 2, "um tipo 1 do início e um do giro")
    }
}

/// O iPhone ocupado diz de quem é a vaga (o id), para a extensão não bater na porta de um aparelho que é dela mesma.
final class RecusaOcupadoTests: XCTestCase {
    func testOcupadoLevaOIdDoAparelho() throws {
        let d = Handshake.respostaErro("ocupado", id: "ID-ABC")
        let j = try XCTUnwrap(JSONSerialization.jsonObject(with: d.dropLast()) as? [String: Any])
        XCTAssertEqual(j["ok"] as? Bool, false)
        XCTAssertEqual(j["erro"] as? String, "ocupado")
        XCTAssertEqual(j["id"] as? String, "ID-ABC")
    }

    func testOutrosErrosNaoLevamId() throws {
        for e in ["versao", "handshake", "camera"] {
            let j = try XCTUnwrap(JSONSerialization.jsonObject(with: Handshake.respostaErro(e, id: "ID-ABC").dropLast()) as? [String: Any])
            XCTAssertNil(j["id"], e)
        }
        let semId = try XCTUnwrap(JSONSerialization.jsonObject(with: Handshake.respostaErro("ocupado").dropLast()) as? [String: Any])
        XCTAssertNil(semId["id"])
    }
}

/// Desenha a tela inteira em pé e em paisagem, sem janela visível na mesa, e guarda os PNG em FMCAM_LAYOUT_DIR (se dado)
/// para conferir o layout a olho. O teste em si só garante que a tela desenha e que em pé ela não fica vazia.
@MainActor
final class TelaDesenhoTests: XCTestCase {
    private func desenhar(_ m: ModeloApp, _ tam: CGSize) -> UIImage {
        let host = UIHostingController(rootView: TelaPrincipal(modelo: m))
        let janela = UIWindow(frame: CGRect(origin: .zero, size: tam))
        janela.rootViewController = host
        janela.isHidden = false
        host.view.frame = janela.bounds
        host.view.layoutIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.35))
        host.view.layoutIfNeeded()
        let img = UIGraphicsImageRenderer(size: tam).image { _ in
            host.view.drawHierarchy(in: CGRect(origin: .zero, size: tam), afterScreenUpdates: true)
        }
        janela.isHidden = true
        return img
    }

    private func modelo(emPe: Bool) -> ModeloApp {
        let m = ModeloApp()
        m.prefs = PreferenciasTela()
        m.dimCamera = emPe ? DimensoesVideo(largura: 1080, altura: 1920, fps: 60) : DimensoesVideo(largura: 1920, altura: 1080, fps: 60)
        m.anguloFisico = emPe ? 90 : 0
        m.lentes = OpcoesLente.montar([InfoLente(lente: .ultra, fov: 120), InfoLente(lente: .wide, fov: 69), InfoLente(lente: .tele, fov: 15.6)])
        m.bateria = 0.82
        return m
    }

    func testBotoesDoTrilhoEmPeTemTodosAMesmaLarguraEOcupamALinha() {
        let disp: CGFloat = 394
        let l = MedidasTela.larguraItemEmPe(disponivel: disp, itens: 6)
        XCTAssertEqual(l * 6 + MedidasTela.espacoTrilhoEmPe * 5, disp, accuracy: 6)
        XCTAssertEqual(MedidasTela.larguraItemEmPe(disponivel: disp, itens: 8), MedidasTela.itemEmPeMinimo)  // rola de lado
        XCTAssertEqual(MedidasTela.larguraItemEmPe(disponivel: disp, itens: 0), MedidasTela.itemEmPeMinimo)
    }

    func testProporcaoVemDoLadoENaoDaDimensaoAtrasada() {
        let d = DimensoesVideo(largura: 3840, altura: 2160, fps: 30), t = DimensoesVideo(largura: 2160, altura: 3840, fps: 30)
        for x in [d, t, nil] {
            XCTAssertEqual(MedidasTela.proporcao(x, emPe: false), 16.0 / 9.0, accuracy: 0.001)
            XCTAssertEqual(MedidasTela.proporcao(x, emPe: true), 9.0 / 16.0, accuracy: 0.001)
        }
    }

    /// Ao girar, a janela vira na hora e o tamanho do vídeo só chega depois do debounce e da troca do codificador. Nesse
    /// intervalo a tela tem que sair igual à definitiva (os controles iam para um retângulo estreito no meio da tela).
    func testGirarNaoDesmontaATelaEnquantoOVideoNaoTroca() throws {
        let deitada = CGSize(width: 932, height: 430), emPe = CGSize(width: 430, height: 932)
        for (nome, tam, pe) in [("deitar", deitada, false), ("levantar", emPe, true)] {
            for (caso, ajuste) in [("ajustes", { (m: ModeloApp) in m.grupo = .ajustes }), ("noar", { (m: ModeloApp) in m.noArForcado = true }),
                                   ("guias", { (m: ModeloApp) in m.ajustes.grade = true; m.ajustes.centro = true })] {
                let certa = modelo(emPe: pe), girando = modelo(emPe: pe)
                girando.dimCamera = modelo(emPe: !pe).dimCamera
                ajuste(certa); ajuste(girando)
                let a = desenhar(certa, tam).pngData(), b = desenhar(girando, tam).pngData()
                if let p = ProcessInfo.processInfo.environment["FMCAM_LAYOUT_DIR"], let b {
                    try? FileManager.default.createDirectory(atPath: p, withIntermediateDirectories: true)
                    try b.write(to: URL(fileURLWithPath: "\(p)/giro-\(nome)-\(caso).png"))
                }
                XCTAssertEqual(a, b, "\(nome) \(caso)")
            }
        }
    }

    func testDesenhaTodosOsEstadosEmPeEEmPaisagem() throws {
        let pasta = ProcessInfo.processInfo.environment["FMCAM_LAYOUT_DIR"]
        if let p = pasta { try? FileManager.default.createDirectory(atPath: p, withIntermediateDirectories: true) }
        let tamanhos: [(String, CGSize, Bool)] = [("pe", CGSize(width: 430, height: 932), true), ("pe-pequeno", CGSize(width: 375, height: 667), true),
                                                   ("paisagem", CGSize(width: 932, height: 430), false)]
        for (nome, tam, emPe) in tamanhos {
            let casos: [(String, (ModeloApp) -> Void)] = [
                ("base", { _ in }),
                ("camera", { $0.grupo = .camera }),
                ("info-todos", { m in m.grupo = .info; m.prefs.topo = Set(ItemTopo.allCases) }),
                ("ajustes", { $0.grupo = .ajustes }),
                ("qualidade", { m in m.grupo = .ajustes; m.painel = .qualidade }),
                ("foco", { m in m.grupo = .camera; m.painel = .foco }),
                ("exposicao-manual", { m in m.grupo = .camera; m.painel = .exposicao; m.ajustes.exposicaoModo = .manual }),
                ("noar", { m in m.noArForcado = true; m.grupo = .camera }),
                ("lado-direita", { m in m.prefs.lado = .direita; m.grupo = .guias }),
                ("limpo-noar", { m in m.limpo = true; m.noArForcado = true }),
                ("aviso", { m in m.mostrarAviso("Foco travado") }),
                ("guias", { m in m.ajustes.grade = true; m.ajustes.centro = true; m.ajustes.nivel = true; m.anguloNivel = 6 }),
            ]
            for (caso, ajuste) in casos {
                let m = modelo(emPe: emPe)
                ajuste(m)
                let img = desenhar(m, tam)
                XCTAssertGreaterThan(img.size.width, 0)
                if let p = pasta, let png = img.pngData() {
                    try png.write(to: URL(fileURLWithPath: "\(p)/\(nome)-\(caso).png"))
                }
            }
        }
    }
}
