import SwiftUI
import XCTest
@testable import FMCam

final class CicloTests: XCTestCase {
    func testInactiveNaoMexeNaCamera() {
        // teclado, sheet, Central de Controle, notificação e banner de ligação deixam o app em inactive
        XCTAssertNil(CicloDeVida.acao(para: .inactive))
    }
    func testActiveLiga() { XCTAssertEqual(CicloDeVida.acao(para: .active), .ligar) }
    func testSoBackgroundDesliga() { XCTAssertEqual(CicloDeVida.acao(para: .background), .desligar) }
}

final class EstabilizacaoTests: XCTestCase {
    func testEscolheOModoDeMenorAtraso() {
        XCTAssertEqual(Estabilizacao.escolher(suportados: [.standard, .previewOptimized]), .standard)
        XCTAssertEqual(Estabilizacao.escolher(suportados: [.previewOptimized]), .previewOptimized)
        XCTAssertEqual(Estabilizacao.escolher(suportados: [.standard]), .standard)
    }

    func testSemModoSuportadoNaoEscolheNada() {
        XCTAssertNil(Estabilizacao.escolher(suportados: []))
    }

    func testAlternarEstabilizacaoNaoMudaLenteNemZoom() {
        var a = AjustesCamera()
        a.lente = .tele; a.zoom = 3
        for ligar in [true, false, true] {
            a.estabilizar = ligar
            XCTAssertEqual(a.lente, .tele)
            XCTAssertEqual(a.zoom, 3)
        }
    }

    func testReconexaoSemLenteNoPedidoNaoDevolveParaA1x() {
        var a = AjustesCamera()
        a.lente = .tele; a.zoom = 1
        // OBS sem o campo "lente": quem escolheu foi o iPhone
        guard case .aceito(let p) = Handshake.interpretar(linha: Data(#"{"v":1}"#.utf8)) else { return XCTFail("handshake") }
        XCTAssertFalse(p.lenteInformada)
        XCTAssertEqual(a.aplicando(config: ConfigVideo(p)).lente, .tele)
        // OBS pedindo wide de propósito: vale
        guard case .aceito(let q) = Handshake.interpretar(linha: Data(#"{"v":1,"lente":"wide"}"#.utf8)) else { return XCTFail("handshake") }
        XCTAssertTrue(q.lenteInformada)
        XCTAssertEqual(a.aplicando(config: ConfigVideo(q)).lente, .wide)
        // valor inválido de lente não conta como pedido
        guard case .aceito(let r) = Handshake.interpretar(linha: Data(#"{"v":1,"lente":"xx"}"#.utf8)) else { return XCTFail("handshake") }
        XCTAssertFalse(r.lenteInformada)
    }

    func testConfigVideoLenteExplicita() {
        XCTAssertTrue(ConfigVideo(lente: .tele).lenteInformada)
        XCTAssertFalse(ConfigVideo().lenteInformada)
    }
}

final class AssinaturaTests: XCTestCase {
    /// CMS de mentira: lixo binário, plist XML no meio, lixo no fim.
    func perfilFalso(data: String) -> Data {
        var d = Data([0x30, 0x82, 0x0A, 0xFF, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x00, 0xFF, 0xFE])
        d.append(Data("""
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0"><dict><key>Name</key><string>iOS Team Provisioning Profile</string>
        <key>ExpirationDate</key><date>\(data)</date><key>TeamName</key><string>Teste</string></dict></plist>
        """.utf8))
        d.append(Data([0x00, 0xFF, 0x13, 0x37, 0x80, 0x00]))
        return d
    }

    func iso(_ s: String) -> Date? { ISO8601DateFormatter().date(from: s) }

    func testExtraiAData() {
        let esperado = iso("2026-11-25T12:30:00Z")
        XCTAssertNotNil(esperado)
        XCTAssertEqual(Assinatura.dataDeExpiracao(dados: perfilFalso(data: "2026-11-25T12:30:00Z")), esperado)
    }

    func testLeDeArquivo() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("perfil-\(UUID().uuidString).mobileprovision")
        try perfilFalso(data: "2027-01-02T03:04:05Z").write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertEqual(Assinatura.dataDeExpiracao(arquivo: url), iso("2027-01-02T03:04:05Z"))
    }

    func testAusenteOuQuebradoNaoDerrubaNada() {
        XCTAssertNil(Assinatura.dataDeExpiracao(arquivo: URL(fileURLWithPath: "/nao/existe.mobileprovision")))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data()))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data([0xFF, 0x00, 0x13])))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data("<?xml sem fim".utf8)))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data("</plist> antes <?xml".utf8)))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data("<?xml version=\"1.0\"?><plist><dict><key>A</key><string>b</plist>".utf8)))
        XCTAssertNil(Assinatura.dataDeExpiracao(dados: Data("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>Name</key><string>x</string></dict></plist>".utf8)), "sem ExpirationDate")
        XCTAssertNil(Assinatura.doBundle(Bundle(for: AssinaturaTests.self)), "no simulador não há perfil")
    }

    func testTextoEUrgencia() throws {
        let utc = try XCTUnwrap(TimeZone(identifier: "UTC"))
        let vence = try XCTUnwrap(iso("2026-11-25T12:30:00Z"))
        let longe = Assinatura.texto(vence: vence, agora: vence.addingTimeInterval(-5 * 86400 - 3600), fuso: utc)
        XCTAssertEqual(longe.texto, "Assinatura vence em 6 dias (25/11 12:30)", "5 d 1 h arredonda para cima, como o SideStore")
        XCTAssertFalse(longe.urgente)
        let doisDias = Assinatura.texto(vence: vence, agora: vence.addingTimeInterval(-2 * 86400), fuso: utc)
        XCTAssertTrue(doisDias.urgente, "2 dias ou menos fica amarelo")
        XCTAssertEqual(doisDias.texto, "Assinatura vence em 2 dias (25/11 12:30)")
        let umDia = Assinatura.texto(vence: vence, agora: vence.addingTimeInterval(-86400 + 60), fuso: utc)
        XCTAssertEqual(umDia.texto, "Assinatura vence em 1 dia (25/11 12:30)")
        XCTAssertEqual(Assinatura.texto(vence: vence, agora: vence.addingTimeInterval(-3600), fuso: utc).texto, "Assinatura vence em 1 dia (25/11 12:30)")
        let vencida = Assinatura.texto(vence: vence, agora: vence.addingTimeInterval(60), fuso: utc)
        XCTAssertEqual(vencida.texto, "Assinatura vencida em 25/11 12:30")
        XCTAssertTrue(vencida.urgente)
    }

    func testCampoNoStatus() throws {
        let vence = try XCTUnwrap(iso("2026-11-25T12:30:00Z"))
        let c = StatusCamera.campos(ajustes: AjustesCamera(), leitura: LeituraCamera(), noAr: false, dim: nil, assinaturaVence: vence)
        XCTAssertEqual(c["assinatura_vence"] as? String, "2026-11-25T12:30:00Z")
        XCTAssertNil(StatusCamera.campos(ajustes: AjustesCamera(), leitura: LeituraCamera(), noAr: false, dim: nil)["assinatura_vence"])
    }
}

final class FocoTests: XCTestCase {
    func defaults() -> (UserDefaults, String) {
        let n = "teste-foco-\(UUID().uuidString)"
        return (UserDefaults(suiteName: n) ?? .standard, n)
    }

    func testOsModosSaoSoAutoEManual() {
        XCTAssertEqual(ModoFoco.allCases, [.auto, .manual])
        XCTAssertEqual(AjustesCamera().focoModo, .auto)
    }

    func testValorSalvoContinuoMigraParaAuto() {
        let (d, n) = defaults()
        d.set(Data(#"{"focoModo":"continuo","focoTravado":true}"#.utf8), forKey: AjustesCamera.chave)
        let a = AjustesCamera.carregar(defaults: d)
        XCTAssertEqual(a.focoModo, .auto)
        XCTAssertTrue(a.focoTravado, "o resto do ajuste continua lendo")
        // ao salvar de novo já sai como auto
        a.salvar(defaults: d)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d).focoModo, .auto)
        d.removePersistentDomain(forName: n)
    }

    func testValoresDesconhecidosEManualSaoLidosCerto() {
        let (d, n) = defaults()
        d.set(Data(#"{"focoModo":"manual"}"#.utf8), forKey: AjustesCamera.chave)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d).focoModo, .manual)
        d.set(Data(#"{"focoModo":"qualquer"}"#.utf8), forKey: AjustesCamera.chave)
        XCTAssertEqual(AjustesCamera.carregar(defaults: d).focoModo, .auto)
        d.removePersistentDomain(forName: n)
    }

    func testAutoEContinuoComMonitoramentoDeCena() {
        let p = PlanoDeFoco.de(AjustesCamera())
        XCTAssertEqual(p.modo, .continuo)
        XCTAssertTrue(p.monitorarCena)
        XCTAssertTrue(p.centralizar)
    }

    func testTravadoEManualNaoMonitoramNemVoltamAoCentro() {
        var a = AjustesCamera()
        a.focoTravado = true
        XCTAssertEqual(PlanoDeFoco.de(a).modo, .travado)
        XCTAssertFalse(PlanoDeFoco.devolveAoCentro(a), "travado não sai do ponto")
        a.focoTravado = false; a.focoModo = .manual; a.posicaoLente = 0.7
        let m = PlanoDeFoco.de(a)
        XCTAssertEqual(m.modo, .manual)
        XCTAssertEqual(m.posicaoLente, 0.7)
        XCTAssertFalse(m.monitorarCena)
        XCTAssertFalse(PlanoDeFoco.devolveAoCentro(a))
    }

    func testPosicaoManualFicaNaFaixa() {
        var a = AjustesCamera()
        a.focoModo = .manual; a.posicaoLente = 5
        XCTAssertEqual(PlanoDeFoco.de(a).posicaoLente, 1)
    }

    /// O plano só depende dos ajustes: trocar lente, zoom ou estabilização e reaplicar dá o mesmo foco.
    func testReconfigurarNaoMudaOPlanoDeFoco() {
        var a = AjustesCamera()
        let antes = PlanoDeFoco.de(a)
        a.lente = .tele; a.zoom = 3; a.estabilizar = true; a.bias = 1; a.grade.toggle()
        XCTAssertEqual(PlanoDeFoco.de(a), antes)
        a.focoModo = .manual
        let manual = PlanoDeFoco.de(a)
        a.lente = .ultra; a.estabilizar = false
        XCTAssertEqual(PlanoDeFoco.de(a), manual)
    }

    func testStatusFocoUsaAutoManualTravado() {
        var a = AjustesCamera()
        let l = LeituraCamera()
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "auto")
        a.focoTravado = true
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "travado")
        a.focoModo = .manual
        XCTAssertEqual(StatusCamera.campos(ajustes: a, leitura: l, noAr: false, dim: nil)["foco"] as? String, "manual")
    }
}
