import XCTest
@testable import FMCam

/// Crash de 24/11/2026 (iPhone 17 Pro Max, iOS 26.6.2): "Trava BB" derrubava o app com NSException em
/// setWhiteBalanceModeLockedWithDeviceWhiteBalanceGains. O app salvava a trava e caía de novo a cada abertura.
final class LimitesDeCameraTests: XCTestCase {
    /// A conta de antes: min(max(v, 1), m). Com m menor que 1 (maxWhiteBalanceGain ainda 0 antes da sessão rodar) devolve
    /// valor fora de 1...max, e com NaN devolve NaN: os dois lançam NSException no AVFoundation.
    private func contaAntiga(_ v: Float, max m: Float) -> Float { min(max(v, 1), m) }

    func testContaAntigaDavaGanhoInvalido() {
        XCTAssertEqual(contaAntiga(1.5, max: 0), 0, "máximo zero (sessão parada): 0 < 1, fora da faixa")
        XCTAssertTrue(contaAntiga(.nan, max: 4).isNaN, "NaN passava direto")
    }

    func testGanhosSeguros() {
        XCTAssertEqual(LimitesDeCamera.ganhosSeguros(r: 1.5, g: 1, b: 2.2, max: 4), .init(r: 1.5, g: 1, b: 2.2))
        XCTAssertEqual(LimitesDeCamera.ganhosSeguros(r: 9, g: 0.2, b: -3, max: 4), .init(r: 4, g: 1, b: 1), "limita a 1...max")
        // não finito (NaN, infinito) cai no padrão 1: nunca vira ganho inventado
        XCTAssertEqual(LimitesDeCamera.ganhosSeguros(r: .nan, g: .infinity, b: -.infinity, max: 4), .init(r: 1, g: 1, b: 1))
    }

    func testGanhosSemMaximoValidoNaoExistem() {
        XCTAssertNil(LimitesDeCamera.ganhosSeguros(r: 1.5, g: 1.5, b: 1.5, max: 0), "sessão parada: máximo 0")
        XCTAssertNil(LimitesDeCamera.ganhosSeguros(r: 1.5, g: 1.5, b: 1.5, max: .nan))
        XCTAssertNil(LimitesDeCamera.ganhosSeguros(r: 1.5, g: 1.5, b: 1.5, max: 0.99))
    }

    func testTodoGanhoSeguroFicaEm1AteMax() {
        for m: Float in [1, 2, 3.99, 4, 8] {
            for v: Float in [-5, 0, 0.99, 1, 2.5, 7, 1e9, .nan, .infinity, -.infinity] {
                guard let g = LimitesDeCamera.ganhosSeguros(r: v, g: v, b: v, max: m) else { return XCTFail("m=\(m)") }
                for x in [g.r, g.g, g.b] { XCTAssertTrue(x >= 1 && x <= m, "v=\(v) m=\(m) deu \(x)") }
            }
        }
    }

    func testPlanoDoBranco() {
        let g = LimitesDeCamera.Ganhos(r: 1.2, g: 1, b: 1.8)
        XCTAssertEqual(PlanoBranco.decidir(suportaGanhos: true, suportaTravado: true, ganhos: g), .ganhos(g))
        // dispositivo virtual sem ganhos custom: trava pelo modo
        XCTAssertEqual(PlanoBranco.decidir(suportaGanhos: false, suportaTravado: true, ganhos: nil), .modoTravado)
        // ganhos suportados mas máximo inválido: também pelo modo, nunca com ganho inventado
        XCTAssertEqual(PlanoBranco.decidir(suportaGanhos: true, suportaTravado: true, ganhos: nil), .modoTravado)
        // nem um nem outro: só ignora
        if case .ignorar = PlanoBranco.decidir(suportaGanhos: false, suportaTravado: false, ganhos: nil) {} else { XCTFail() }
        if case .ignorar = PlanoBranco.decidir(suportaGanhos: true, suportaTravado: false, ganhos: nil) {} else { XCTFail() }
    }

    func testKelvinSeguro() {
        XCTAssertEqual(LimitesDeCamera.kelvinSeguro(5500), 5500)
        XCTAssertEqual(LimitesDeCamera.kelvinSeguro(100), 2000)
        XCTAssertEqual(LimitesDeCamera.kelvinSeguro(99999), 10000)
        XCTAssertEqual(LimitesDeCamera.kelvinSeguro(.nan), 5500)
    }

    func testLimitarGeral() {
        XCTAssertEqual(LimitesDeCamera.limitar(5.0, 0, 10, padrao: 1), 5)
        XCTAssertEqual(LimitesDeCamera.limitar(Double(-5), 0, 10, padrao: 1), 0)
        XCTAssertEqual(LimitesDeCamera.limitar(Double(50), 0, 10, padrao: 1), 10)
        XCTAssertEqual(LimitesDeCamera.limitar(Double.nan, 0, 10, padrao: 3), 3)
        XCTAssertEqual(LimitesDeCamera.limitar(Double.nan, 0, 10, padrao: 30), 10, "o padrão também é limitado")
        XCTAssertEqual(LimitesDeCamera.limitar(Double(5), 8, 2, padrao: 1), 8, "faixa invertida vale o piso")
        XCTAssertEqual(LimitesDeCamera.limitar(Double(5), .nan, 2, padrao: 1), 1)
        XCTAssertEqual(LimitesDeCamera.limitarF(7, 1, 4, padrao: 1), 4)
    }

    func testObturadorNuncaFicaAbaixoDoPisoDoFormato() {
        // duração do quadro (1/60) menor que o piso do formato (1/30 s?): o teto não pode ficar abaixo do piso
        let r = LimitesDeCamera.obturador(0.5, min: 0.001, max: 0.5, duracaoQuadro: 0.0005, padrao: 0.01)
        XCTAssertGreaterThanOrEqual(r, 0.001)
        XCTAssertEqual(LimitesDeCamera.obturador(1.0 / 60, min: 1.0 / 32000, max: 1.0 / 3, duracaoQuadro: 1.0 / 30, padrao: 0.01), 1.0 / 60, accuracy: 1e-9)
        XCTAssertEqual(LimitesDeCamera.obturador(1, min: 1.0 / 32000, max: 1.0 / 3, duracaoQuadro: 1.0 / 30, padrao: 0.01), 1.0 / 30, accuracy: 1e-9, "teto = 1 quadro")
        XCTAssertEqual(LimitesDeCamera.obturador(.nan, min: 0.001, max: 0.1, duracaoQuadro: .nan, padrao: 0.02), 0.02, accuracy: 1e-9)
        XCTAssertEqual(LimitesDeCamera.obturador(-1, min: 0.001, max: 0.1, duracaoQuadro: 0.03, padrao: 0.02), 0.001, accuracy: 1e-9)
    }

    func testZoomDentroDoDispositivo() {
        XCTAssertEqual(LimitesDeCamera.zoom(3, min: 1, max: 5), 3)
        XCTAssertEqual(LimitesDeCamera.zoom(0.2, min: 1, max: 5), 1)
        XCTAssertEqual(LimitesDeCamera.zoom(20, min: 1, max: 5), 5)
        XCTAssertEqual(LimitesDeCamera.zoom(.nan, min: 1, max: 5), 1)
        XCTAssertEqual(LimitesDeCamera.zoom(4, min: 2, max: 1.5), 2, "máximo menor que o mínimo")
        XCTAssertEqual(LimitesDeCamera.zoom(0.7, min: 0.5, max: 5), 1, "nunca abaixo de 1 aqui (a faixa do app começa em 1)")
    }

    func testPontoDeInteresseFicaEmZeroUm() {
        XCTAssertEqual(LimitesDeCamera.ponto(CGPoint(x: 1.4, y: -0.2)), CGPoint(x: 1, y: 0))
        XCTAssertEqual(LimitesDeCamera.ponto(CGPoint(x: .nan, y: 0.3)), CGPoint(x: 0.5, y: 0.3))
    }
}

final class ProtecaoDeCrashTests: XCTestCase {
    private func defaults() throws -> UserDefaults { try XCTUnwrap(UserDefaults(suiteName: "fmcam.crash.\(UUID().uuidString)")) }

    func testMarcaQueSobrouDerrubaAsTravasUmaVez() throws {
        let d = try defaults()
        let p = ProtecaoDeCrash(defaults: d)
        p.marcar()  // o app morreu dentro de aplicarAjustes: ninguém limpou
        XCTAssertTrue(ProtecaoDeCrash(defaults: d).consumir())
        XCTAssertFalse(ProtecaoDeCrash(defaults: d).consumir(), "só na abertura seguinte ao crash")
    }

    func testAplicacaoQueTerminouNaoDeixaMarca() throws {
        let d = try defaults()
        let p = ProtecaoDeCrash(defaults: d)
        p.marcar()
        p.limpar()
        XCTAssertFalse(p.consumir())
        XCTAssertFalse(ProtecaoDeCrash(defaults: try defaults()).consumir(), "primeira abertura")
    }

    func testSemTravasZeraSoOQueTrava() {
        var a = AjustesCamera()
        a.brancoTravado = true; a.kelvin = 5200; a.focoTravado = true; a.exposicaoTravada = true
        a.focoModo = .manual; a.exposicaoModo = .manual; a.bias = 1.5; a.zoom = 4
        a.preset = .p2160_30; a.bitrate = 30_000_000; a.lente = .tele; a.grade = false; a.estabilizar = true
        let z = a.semTravas()
        XCTAssertFalse(z.brancoTravado); XCTAssertNil(z.kelvin); XCTAssertFalse(z.focoTravado); XCTAssertFalse(z.exposicaoTravada)
        XCTAssertEqual(z.focoModo, .auto); XCTAssertEqual(z.exposicaoModo, .auto); XCTAssertEqual(z.bias, 0); XCTAssertEqual(z.zoom, 1)
        XCTAssertEqual(z.preset, .p2160_30); XCTAssertEqual(z.bitrate, 30_000_000); XCTAssertEqual(z.lente, .tele)
        XCTAssertFalse(z.grade); XCTAssertTrue(z.estabilizar)
    }

    func testAjustesSalvosComLixoNaoViramValorInvalido() throws {
        let d = try defaults()
        d.set(Data(#"{"kelvin":1e30,"iso":1e30,"obturador":-4,"posicaoLente":9,"zoom":-3,"bias":99}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.kelvin, 10000)
        XCTAssertEqual(a.iso, 12800)
        XCTAssertEqual(a.obturador, 1.0 / 32000, accuracy: 1e-12)
        XCTAssertEqual(a.posicaoLente, 1)
        XCTAssertEqual(a.zoom, 1)
        XCTAssertEqual(a.bias, 3)
    }
}
