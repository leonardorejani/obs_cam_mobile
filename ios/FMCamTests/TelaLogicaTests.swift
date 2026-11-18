import XCTest
@testable import FMCam

final class NivelTests: XCTestCase {
    func rad(_ g: Double) -> Double { g * .pi / 180 }

    func testNivelEmPaisagemEsquerda() {
        // topo do aparelho para a esquerda: gravidade no eixo x negativo
        XCTAssertEqual(Nivel.angulo(gx: -1, gy: 0) ?? 99, 0, accuracy: 0.001)
        XCTAssertEqual(Nivel.angulo(gx: -cos(rad(10)), gy: sin(rad(10))) ?? 99, 10, accuracy: 0.001)
        XCTAssertEqual(Nivel.angulo(gx: -cos(rad(10)), gy: -sin(rad(10))) ?? 99, -10, accuracy: 0.001)
    }

    func testNivelEmPaisagemDireitaTemSinalEspelhado() {
        XCTAssertEqual(Nivel.angulo(gx: 1, gy: 0) ?? 99, 0, accuracy: 0.001)
        XCTAssertEqual(Nivel.angulo(gx: cos(rad(10)), gy: sin(rad(10))) ?? 99, -10, accuracy: 0.001)
        XCTAssertEqual(Nivel.angulo(gx: cos(rad(10)), gy: -sin(rad(10))) ?? 99, 10, accuracy: 0.001)
    }

    func testMesmaInclinacaoFisicaDaOMesmoSinalNasDuasPaisagens() {
        // girar o aparelho 180 graus no plano troca os sinais de gx e gy e o horizonte aparente continua o mesmo
        let esquerda = Nivel.angulo(gx: -cos(rad(7)), gy: sin(rad(7)))
        let direita = Nivel.angulo(gx: cos(rad(7)), gy: -sin(rad(7)))
        XCTAssertEqual(esquerda ?? 99, direita ?? -99, accuracy: 0.001)
    }

    func testEmPeOuDeitadoNaoTemNivel() {
        XCTAssertNil(Nivel.angulo(gx: 0, gy: -1), "retrato")
        XCTAssertNil(Nivel.angulo(gx: 0.1, gy: 0.2), "deitado na mesa")
        XCTAssertNil(Nivel.angulo(gx: 0.5, gy: 0), "limite")
        XCTAssertNotNil(Nivel.angulo(gx: 0.51, gy: 0))
    }

    func testHisteresePerMiteNivelarSemPiscar() {
        var n = Nivelamento()
        XCTAssertFalse(n.atualizar(angulo: 3), "longe: não nivela")
        XCTAssertFalse(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: 1.2), "entre 1 e 1,5 fora do nivelado continua fora")
        XCTAssertTrue(n.atualizar(angulo: 0.8), "menos de 1 grau: entra nivelado (e avisa uma vez)")
        XCTAssertTrue(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: 0.9), "continua nivelado, sem avisar de novo")
        XCTAssertFalse(n.atualizar(angulo: 1.3), "entre 1 e 1,5 dentro do nivelado: segura")
        XCTAssertTrue(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: -1.4))
        XCTAssertTrue(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: 1.6), "passou de 1,5: sai")
        XCTAssertFalse(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: 1.2), "e só volta a menos de 1 grau")
        XCTAssertTrue(n.atualizar(angulo: -0.5))
    }

    func testSemLeituraDesnivela() {
        var n = Nivelamento()
        _ = n.atualizar(angulo: 0)
        XCTAssertTrue(n.nivelado)
        XCTAssertFalse(n.atualizar(angulo: nil))
        XCTAssertFalse(n.nivelado)
    }
}

final class ModoLimpoTests: XCTestCase {
    func testModoNormalSegueOsToggles() {
        var a = AjustesCamera()
        a.grade = true; a.centro = false; a.nivel = true
        let e = ModoLimpo.elementos(limpo: false, ajustes: a, noAr: false)
        XCTAssertEqual(e, ElementosVisiveis(controles: true, grade: true, cruz: false, nivel: true, tally: false))
    }

    func testModoLimpoEscondeTudoMenosGuiasETally() {
        var a = AjustesCamera()
        a.grade = false; a.centro = false; a.nivel = false
        let e = ModoLimpo.elementos(limpo: true, ajustes: a, noAr: true)
        XCTAssertEqual(e, ElementosVisiveis(controles: false, grade: true, cruz: true, nivel: true, tally: true),
                       "limpo mostra grade, cruz e nível mesmo com os toggles desligados, e o tally continua")
        XCTAssertFalse(ModoLimpo.elementos(limpo: true, ajustes: a, noAr: false).tally)
    }

    func testTallyAparecePorForaDoModoLimpo() {
        XCTAssertTrue(ModoLimpo.elementos(limpo: false, ajustes: AjustesCamera(), noAr: true).tally)
    }

    func testTogglesDeGuiasPersistem() {
        let n = "teste-guias-\(UUID().uuidString)"
        guard let d = UserDefaults(suiteName: n) else { return XCTFail("sem defaults") }
        XCTAssertFalse(AjustesCamera().nivel, "nível começa desligado")
        var a = AjustesCamera()
        a.grade = false; a.centro = true; a.nivel = true
        a.salvar(defaults: d)
        let lido = AjustesCamera.carregar(defaults: d)
        XCTAssertFalse(lido.grade)
        XCTAssertTrue(lido.centro)
        XCTAssertTrue(lido.nivel)
        d.removePersistentDomain(forName: n)
    }

    func testAjusteAntigoSemNivelContinuaLendo() {
        let n = "teste-guias2-\(UUID().uuidString)"
        guard let d = UserDefaults(suiteName: n) else { return XCTFail("sem defaults") }
        d.set(Data(#"{"grade":false,"centro":true}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertFalse(a.grade)
        XCTAssertTrue(a.centro)
        XCTAssertFalse(a.nivel)
        d.removePersistentDomain(forName: n)
    }
}

final class SeloAssinaturaTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC") ?? .current
    func iso(_ s: String) -> Date { ISO8601DateFormatter().date(from: s) ?? Date(timeIntervalSince1970: 0) }

    func testSeloEQuando() {
        let v = iso("2026-11-25T10:29:00Z")
        // logo após instalar sobram 6 d 23 h e o SideStore mostra "7 DAYS": a pílula arredonda para cima
        let longe = Assinatura.texto(vence: v, agora: v.addingTimeInterval(-(7 * 86400 - 3600)), fuso: utc)
        XCTAssertEqual(longe.selo, "7 dias")
        XCTAssertEqual(longe.quando, "25/11 às 10:29")
        XCTAssertFalse(longe.urgente); XCTAssertFalse(longe.vencida); XCTAssertFalse(longe.menosDeUmDia)
        XCTAssertEqual(Assinatura.texto(vence: v, agora: v.addingTimeInterval(-86400 + 60), fuso: utc).selo, "1 dia")
        XCTAssertEqual(Assinatura.texto(vence: v, agora: v.addingTimeInterval(-86400 - 60), fuso: utc).selo, "2 dias")
        XCTAssertEqual(Assinatura.texto(vence: v, agora: v.addingTimeInterval(-86400 * 3 + 60), fuso: utc).selo, "3 dias")
    }

    func testAmareloComDoisDiasOuMenosEVermelhoVencido() {
        let v = iso("2026-11-25T10:29:00Z")
        XCTAssertTrue(Assinatura.texto(vence: v, agora: v.addingTimeInterval(-2 * 86400), fuso: utc).urgente)
        XCTAssertFalse(Assinatura.texto(vence: v, agora: v.addingTimeInterval(-3 * 86400), fuso: utc).urgente)
        let menos = Assinatura.texto(vence: v, agora: v.addingTimeInterval(-3600), fuso: utc)
        XCTAssertEqual(menos.selo, "1 dia", "menos de 1 dia ainda mostra 1, como o SideStore")
        XCTAssertTrue(menos.menosDeUmDia)
        let vencida = Assinatura.texto(vence: v, agora: v.addingTimeInterval(10), fuso: utc)
        XCTAssertEqual(vencida.selo, "vencida")
        XCTAssertTrue(vencida.vencida)
        XCTAssertTrue(vencida.menosDeUmDia)
    }

    func testPontoNoModoLimpoSoComMenosDeUmDia() {
        let v = iso("2026-11-25T10:29:00Z")
        let longe = Assinatura.texto(vence: v, agora: v.addingTimeInterval(-86400 * 5), fuso: utc)
        let perto = Assinatura.texto(vence: v, agora: v.addingTimeInterval(-3600), fuso: utc)
        XCTAssertFalse(ModoLimpo.pontoDeAssinatura(limpo: true, assinatura: longe))
        XCTAssertTrue(ModoLimpo.pontoDeAssinatura(limpo: true, assinatura: perto))
        XCTAssertFalse(ModoLimpo.pontoDeAssinatura(limpo: false, assinatura: perto), "fora do modo limpo o selo da faixa já aparece")
        XCTAssertFalse(ModoLimpo.pontoDeAssinatura(limpo: true, assinatura: nil))
    }
}

final class DiasParaVencerTests: XCTestCase {
    private let agora = Date(timeIntervalSince1970: 1_800_000_000)

    /// A pílula arredonda para cima (como o SideStore), o atalho para baixo (lado seguro da automação): 6 d 23 h
    /// aparece como "7 dias" na tela e como 6 aqui.
    func testSeisDiasEVinteETresHorasDaSeisNoAtalhoESeteNaPilula() {
        let vence = agora.addingTimeInterval(7 * 86400 - 3600)
        XCTAssertEqual(Assinatura.diasRestantes(vence: vence, agora: agora), 6)
        XCTAssertEqual(Assinatura.texto(vence: vence, agora: agora).selo, "7 dias")
    }

    func testDataFuturaArredondaParaBaixo() {
        XCTAssertEqual(Assinatura.diasRestantes(vence: agora.addingTimeInterval(5.9 * 86400), agora: agora), 5)
        XCTAssertEqual(Assinatura.diasRestantes(vence: agora.addingTimeInterval(2 * 86400), agora: agora), 2)
    }

    func testDataPassadaDaZero() {
        XCTAssertEqual(Assinatura.diasRestantes(vence: agora.addingTimeInterval(-3600), agora: agora), 0)
        XCTAssertEqual(Assinatura.diasRestantes(vence: agora, agora: agora), 0)
    }

    func testMenosDeUmDiaDaZero() {
        XCTAssertEqual(Assinatura.diasRestantes(vence: agora.addingTimeInterval(0.99 * 86400), agora: agora), 0)
    }
}

final class PreferenciasTelaTests: XCTestCase {
    private func defaultsLimpo() throws -> UserDefaults {
        let d = try XCTUnwrap(UserDefaults(suiteName: "fmcam.teste.\(UUID().uuidString)"))
        return d
    }

    func testPadroes() {
        let p = PreferenciasTela()
        XCTAssertEqual(p.lado, .esquerda)
        XCTAssertTrue(p.mostra(.estado) && p.mostra(.nome) && p.mostra(.formato) && p.mostra(.bateria) && p.mostra(.temperatura) && p.mostra(.validade))
        XCTAssertFalse(p.mostra(.iso))
        XCTAssertFalse(p.mostra(.ip))
    }

    func testPersistenciaDeItensELado() throws {
        let d = try defaultsLimpo()
        var p = PreferenciasTela()
        p.topo.remove(.nome)
        p.topo.insert(.ip)
        p.lado = .direita
        p.salvar(defaults: d)
        let lida = PreferenciasTela.carregar(defaults: d)
        XCTAssertEqual(lida, p)
        XCTAssertFalse(lida.mostra(.nome))
        XCTAssertTrue(lida.mostra(.ip))
        XCTAssertEqual(lida.lado, .direita)
    }

    func testSemDadoSalvoUsaPadrao() throws {
        XCTAssertEqual(PreferenciasTela.carregar(defaults: try defaultsLimpo()), PreferenciasTela())
    }

    func testDivisaoDasCapsulas() {
        var p = PreferenciasTela()
        p.topo.insert(.ip)
        XCTAssertEqual(p.itensEsquerda, [.estado, .nome])
        XCTAssertEqual(p.itensDireita, [.formato, .bateria, .temperatura, .validade, .ip])
    }

    func testNoArPorModo() {
        XCTAssertEqual(PosicaoNoAr.de(noAr: false, limpo: false), .nenhuma)
        XCTAssertEqual(PosicaoNoAr.de(noAr: true, limpo: false), .pilulaNoTopo)
        XCTAssertEqual(PosicaoNoAr.de(noAr: true, limpo: true), .seloCentralizado)
    }

    func testRecuosDoPainelEspelham() {
        let e = MedidasTela.recuosDoPainel(lado: .esquerda, trilhoAberto: true)
        XCTAssertEqual(e.esquerda, 188)
        XCTAssertEqual(e.direita, 76)
        let d = MedidasTela.recuosDoPainel(lado: .direita, trilhoAberto: false)
        XCTAssertEqual(d.esquerda, 76)
        XCTAssertEqual(d.direita, 86)
    }

    func testRotulosDinamicos() {
        var a = AjustesCamera()
        XCTAssertEqual(Rotulos.bb(a), "BB")
        XCTAssertEqual(Rotulos.foco(a), "AF")
        XCTAssertEqual(Rotulos.ev(a), "EV")
        a.brancoTravado = true
        a.kelvin = 5200
        a.focoModo = .manual
        a.bias = 0.3
        XCTAssertEqual(Rotulos.bb(a), "5.2K")
        XCTAssertEqual(Rotulos.foco(a), "MF")
        XCTAssertEqual(Rotulos.ev(a), "+0.3")
        a.bias = -1
        XCTAssertEqual(Rotulos.ev(a), "-1.0")
        a.exposicaoModo = .manual
        XCTAssertEqual(Rotulos.ev(a), "ISO")
    }

    func testAreaDoVideoComBarrasPretas() {
        let r = MedidasTela.areaDoVideo(tela: CGSize(width: 932, height: 430), proporcao: 16.0 / 9.0)
        XCTAssertEqual(r.height, 430, accuracy: 0.01)
        XCTAssertEqual(r.width, 430 * 16 / 9, accuracy: 0.01)
        XCTAssertEqual(r.midX, 466, accuracy: 0.01)
    }
}
