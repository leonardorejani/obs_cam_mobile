import XCTest
@testable import FMCam

// Controle remoto pelo OBS (pacote tipo 4): cada comando vale como o toque equivalente na tela.

private func comando(_ json: String) -> ComandoRemoto? {
    guard case .objeto(let m)? = JSONEstrito.interpretar(Data(json.utf8)) else { return nil }
    return ComandoRemoto.interpretar(m)
}

final class ComandoRemotoParserTests: XCTestCase {
    func testComandosValidos() {
        XCTAssertEqual(comando(#"{"cmd":"lente","fator":2}"#), .lente(fator: 2))
        XCTAssertEqual(comando(#"{"cmd":"lente","fator":0.5}"#), .lente(fator: 0.5))
        XCTAssertEqual(comando(#"{"cmd":"ev","valor":-0.7}"#), .ev(-0.7))
        XCTAssertEqual(comando(#"{"cmd":"trava_ae","valor":true}"#), .travaAE(true))
        XCTAssertEqual(comando(#"{"cmd":"trava_af","valor":false}"#), .travaAF(false))
        XCTAssertEqual(comando(#"{"cmd":"trava_bb","valor":true}"#), .travaBB(true))
        XCTAssertEqual(comando(#"{"cmd":"qualidade","preset":"2160p30","bitrate":25000000}"#), .qualidade(preset: .p2160_30, bitrate: 25_000_000))
        XCTAssertEqual(comando(#"{"cmd":"qualidade","preset":"720p60"}"#), .qualidade(preset: .p720_60, bitrate: nil))
        XCTAssertEqual(comando(#"{"cmd":"qualidade","bitrate":8000000}"#), .qualidade(preset: nil, bitrate: 8_000_000))
    }

    func testCampoDesconhecidoEIgnorado() {
        XCTAssertEqual(comando(#"{"cmd":"ev","valor":1,"extra":[1,2,{"a":1}],"x":null}"#), .ev(1))
        XCTAssertEqual(comando(#"{"cmd":"qualidade","preset":"1080p60","novo":true}"#), .qualidade(preset: .p1080_60, bitrate: nil))
    }

    func testDesconhecidoOuInvalidoNaoViraComando() {
        let ruins = [
            #"{"cmd":"zoom","valor":2}"#, #"{"cmd":"lente"}"#, #"{"cmd":"lente","fator":"2"}"#, #"{"cmd":"lente","fator":0}"#,
            #"{"cmd":"lente","fator":-1}"#, #"{"cmd":"ev"}"#, #"{"cmd":"ev","valor":"alto"}"#, #"{"cmd":"ev","valor":true}"#,
            #"{"cmd":"trava_ae"}"#, #"{"cmd":"trava_ae","valor":1}"#, #"{"cmd":"trava_af","valor":"true"}"#, #"{"cmd":"trava_bb","valor":null}"#,
            #"{"cmd":"qualidade"}"#, #"{"cmd":"qualidade","preset":"4k"}"#, #"{"cmd":"qualidade","bitrate":10}"#,
            #"{"cmd":"qualidade","bitrate":99000000}"#, #"{"cmd":"qualidade","bitrate":1.5}"#, #"{"cmd":"qualidade","preset":5}"#,
            #"{"cmd":5,"valor":true}"#, #"{"valor":true}"#,
        ]
        for j in ruins { XCTAssertNil(comando(j), j) }
    }

    func testQualidadeComUmCampoRuimAplicaSoOOutro() {
        XCTAssertEqual(comando(#"{"cmd":"qualidade","preset":"4k","bitrate":20000000}"#), .qualidade(preset: nil, bitrate: 20_000_000))
        XCTAssertEqual(comando(#"{"cmd":"qualidade","preset":"1080p30","bitrate":3}"#), .qualidade(preset: .p1080_30, bitrate: nil))
    }
}

@MainActor
final class ControleRemotoTelaTests: XCTestCase {
    override func setUp() { UserDefaults.standard.removeObject(forKey: NomeCamera.chave) }

    private func novo() -> ModeloApp {
        let m = ModeloApp()
        m.ajustes = AjustesCamera()
        m.lentes = OpcoesLente.montar([InfoLente(lente: .ultra, fov: 120), InfoLente(lente: .wide, fov: 69), InfoLente(lente: .tele, fov: 15.6)])
        return m
    }

    private func botao(_ m: ModeloApp, _ rotulo: String) -> OpcaoLente { m.lentes.first { $0.rotulo == rotulo }! }

    func testBotoesDeLenteDoAparelhoDeTeste() {
        XCTAssertEqual(novo().lentes.compactMap(\.fator), [0.5, 1, 2, 5])
    }

    func testLenteIgualAoToqueNoBotao() {
        for (fator, rotulo) in [(0.5, "0,5x"), (1.0, "1x"), (2.0, "2x"), (5.0, "5x")] {
            let remoto = novo(), toque = novo()
            remoto.aplicarRemoto(.lente(fator: fator))
            toque.selecionar(botao(toque, rotulo))
            XCTAssertEqual(remoto.ajustes, toque.ajustes, rotulo)
            XCTAssertTrue(remoto.estaSelecionada(botao(remoto, rotulo)), "o botão certo acende: \(rotulo)")
            XCTAssertEqual(remoto.caixaAjustes.remoto.fator, fator, "o status fala o botão aceso")
        }
    }

    func testLenteQueOAparelhoNaoTemFicaIgnorada() {
        let m = novo()
        let antes = m.ajustes
        for f in [3.0, 10.0, 0.25, 1.4, .nan, .infinity] { m.aplicarRemoto(.lente(fator: f)) }
        XCTAssertEqual(m.ajustes, antes)
    }

    func testEVIgualAMexerNoControleELimitadoAFaixa() {
        let remoto = novo(), toque = novo()
        remoto.aplicarRemoto(.ev(-0.7))
        toque.definirEV(-0.7)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertEqual(remoto.ajustes.bias, -0.7, accuracy: 1e-9)
        remoto.aplicarRemoto(.ev(9)); XCTAssertEqual(remoto.ajustes.bias, 2, accuracy: 1e-9)
        remoto.aplicarRemoto(.ev(-9)); XCTAssertEqual(remoto.ajustes.bias, -2, accuracy: 1e-9)
        remoto.aplicarRemoto(.ev(0.34)); XCTAssertEqual(remoto.ajustes.bias, 0.3, accuracy: 1e-9, "passo de 0,1 como o controle")
        remoto.aplicarRemoto(.ev(.nan)); XCTAssertEqual(remoto.ajustes.bias, 0.3, accuracy: 1e-9)
    }

    func testEVRespeitaAFaixaDoAparelho() {
        let m = novo()
        let f = LimitesCamera.faixaEV(aparelhoMin: -1, aparelhoMax: 1)
        m.limites = LimitesCamera(evMin: f.min, evMax: f.max)
        m.aplicarRemoto(.ev(2))
        XCTAssertEqual(m.ajustes.bias, 1, accuracy: 1e-9)
        XCTAssertEqual(m.caixaAjustes.remoto.evMin, -1)
        XCTAssertEqual(m.caixaAjustes.remoto.evMax, 1)
        let normal = LimitesCamera.faixaEV(aparelhoMin: -8, aparelhoMax: 8)
        XCTAssertEqual(normal.min, -2); XCTAssertEqual(normal.max, 2)
        let lixo = LimitesCamera.faixaEV(aparelhoMin: .nan, aparelhoMax: 0)
        XCTAssertEqual(lixo.min, -2); XCTAssertEqual(lixo.max, 2)
    }

    func testEVComExposicaoManualEIgnorado() {
        let m = novo()
        m.mudar { $0.exposicaoModo = .manual }
        m.aplicarRemoto(.ev(1.5))
        XCTAssertEqual(m.ajustes.bias, 0, "no manual o controle de EV não existe na tela")
    }

    func testTravasIguaisAoBotaoDoTrilho() {
        let remoto = novo(), toque = novo()
        remoto.aplicarRemoto(.travaAE(true)); toque.alternarTrava(exposicao: true)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertTrue(remoto.ajustes.exposicaoTravada)
        remoto.aplicarRemoto(.travaAF(true)); toque.alternarTrava(exposicao: false)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertTrue(remoto.ajustes.focoTravado)
        remoto.aplicarRemoto(.travaAE(true))   // já estava: não alterna de volta
        XCTAssertTrue(remoto.ajustes.exposicaoTravada)
        remoto.aplicarRemoto(.travaAE(false)); toque.alternarTrava(exposicao: true)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertFalse(remoto.ajustes.exposicaoTravada)
        remoto.aplicarRemoto(.travaAF(false)); XCTAssertFalse(remoto.ajustes.focoTravado)
    }

    func testTravaBBIgualAoPainelBalancoDeBranco() {
        let remoto = novo(), toque = novo()
        remoto.aplicarRemoto(.travaBB(true)); toque.definirTravaBranco(true)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertTrue(remoto.ajustes.brancoTravado)
        XCTAssertNil(remoto.ajustes.kelvin, "trava no valor de agora")
        remoto.mudar { $0.kelvin = 5200 }
        remoto.aplicarRemoto(.travaBB(false)); toque.mudar { $0.kelvin = 5200 }; toque.definirTravaBranco(false)
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
        XCTAssertFalse(remoto.ajustes.brancoTravado)
        XCTAssertNil(remoto.ajustes.kelvin)
    }

    func testQualidadeCaiNoMaisProximoAbaixoQueALenteTem() {
        let m = novo()
        m.presetsSuportados = [.p720_30, .p720_60, .p1080_30, .p1080_60]
        m.aplicarRemoto(.qualidade(preset: .p2160_30, bitrate: 25_000_000))
        XCTAssertEqual(m.ajustes.preset, .p1080_30)
        XCTAssertEqual(m.ajustes.bitrate, 25_000_000)
        m.aplicarRemoto(.qualidade(preset: .p1080_120, bitrate: nil))
        XCTAssertEqual(m.ajustes.preset, .p1080_60)
        m.aplicarRemoto(.qualidade(preset: .p720_60, bitrate: nil))
        XCTAssertEqual(m.ajustes.preset, .p720_60)
        XCTAssertEqual(m.ajustes.bitrate, 25_000_000, "campo ausente não muda")
    }

    func testQualidadeSemFormatoNenhumIgnoraSoOFormato() {
        let m = novo()
        m.presetsSuportados = []
        m.aplicarRemoto(.qualidade(preset: .p2160_30, bitrate: nil))
        XCTAssertEqual(m.ajustes.preset, .p1080_30)
        m.aplicarRemoto(.qualidade(preset: .p2160_30, bitrate: 12_000_000))
        XCTAssertEqual(m.ajustes.preset, .p1080_30)
        XCTAssertEqual(m.ajustes.bitrate, 12_000_000)
    }

    func testQualidadeRemotaIgualAoPainelQualidade() {
        let remoto = novo(), toque = novo()
        remoto.aplicarRemoto(.qualidade(preset: .p1080_60, bitrate: 20_000_000))
        toque.mudar { $0.preset = .p1080_60 }
        toque.mudar { $0.bitrate = 20_000_000 }
        XCTAssertEqual(remoto.ajustes, toque.ajustes)
    }

    func testInfoRemotaNoZoomDePincaNaoAcendeNenhumBotao() {
        let m = novo()
        m.mudar { $0.zoom = 1.5 }
        XCTAssertNil(m.caixaAjustes.remoto.fator)
        XCTAssertEqual(m.caixaAjustes.remoto.lentes, [0.5, 1, 2, 5])
    }
}

final class ControleRemotoServidorTests: XCTestCase {
    var servidor: Servidor?
    override func tearDown() { servidor?.parar(); servidor = nil }

    func subir(caixa: CaixaAjustes, fonte: FonteDeVideo, ajustar: (inout Servidor.Opcoes) -> Void = { _ in }) throws -> UInt16 {
        var op = Servidor.Opcoes()
        op.porta = 0
        op.bonjour = false
        op.timeoutPartidaCamera = 30
        op.intervaloStatus = 60
        ajustar(&op)
        let s = Servidor(opcoes: op, identidade: IdentidadeAparelho(modelo: "iPhone16,2", nome: "Cam", id: "ID-REMOTO"),
                         ajustes: caixa) { fonte }
        servidor = s
        s.iniciar()
        let fim = Date().addingTimeInterval(5)
        while s.portaAtual == nil && Date() < fim { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return try XCTUnwrap(s.portaAtual)
    }

    final class Registro {
        private let trava = NSLock()
        private var _comandos: [ComandoRemoto] = []
        var comandos: [ComandoRemoto] { trava.lock(); defer { trava.unlock() }; return _comandos }
        func anotar(_ c: ComandoRemoto) { trava.lock(); _comandos.append(c); trava.unlock() }
    }

    private func conectar(_ porta: UInt16, _ handshake: String = #"{"v":1}"#) throws -> ClienteTeste {
        let c = ClienteTeste(porta: porta)
        c.enviar(handshake + "\n")
        XCTAssertEqual(try XCTUnwrap(c.lerLinha())["ok"] as? Bool, true)
        return c
    }

    private func comandoRemoto(_ c: ClienteTeste, _ json: String) {
        c.enviar(Pacote(tipo: 4, tempo: 1, dados: Data(json.utf8)).serializar())
    }

    private func status(_ c: ClienteTeste) -> [[String: Any]] {
        c.pacotes.filter { $0.tipo == 3 }.compactMap { try? JSONSerialization.jsonObject(with: $0.dados) as? [String: Any] }
    }

    func testStatusImediatoAposComandoComOsCamposNovos() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let porta = try subir(caixa: caixa, fonte: FonteMuda())
        servidor?.aoComandoRemoto = { c, pronto in
            // o app troca o estado na thread principal e só então chama pronto
            DispatchQueue.main.async {
                if case .travaAE(let v) = c { var a = caixa.valor; a.exposicaoTravada = v; caixa.valor = a }
                caixa.remoto = InfoRemota(lentes: [0.5, 1, 2, 5], fator: 2, evMin: -2, evMax: 2)
                pronto()
            }
        }
        let c = try conectar(porta)
        XCTAssertTrue(c.esperarPacotes(2))
        XCTAssertTrue(status(c).isEmpty, "sem comando o primeiro status só vem em 60 s")
        comandoRemoto(c, #"{"cmd":"trava_ae","valor":true}"#)
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 3 } }, "status imediato não chegou")
        let j = try XCTUnwrap(status(c).first)
        XCTAssertEqual(j["trava_ae"] as? Bool, true)
        XCTAssertEqual(j["trava_af"] as? Bool, false)
        XCTAssertEqual(j["trava_bb"] as? Bool, false)
        XCTAssertEqual(j["lentes"] as? [Double], [0.5, 1, 2, 5])
        XCTAssertEqual(j["fator"] as? Double, 2)
        XCTAssertEqual(j["ev_min"] as? Double, -2)
        XCTAssertEqual(j["ev_max"] as? Double, 2)
        XCTAssertEqual(j["exposicao"] as? String, "travada")
        XCTAssertNotNil(j["bateria"]); XCTAssertNotNil(j["temp"]); XCTAssertNotNil(j["kbps"])
        let linha = String(data: try JSONSerialization.data(withJSONObject: j, options: [.sortedKeys]), encoding: .utf8) ?? ""
        print("STATUS_IMEDIATO_JSON: \(linha)")
    }

    func testComandoInvalidoOuDesconhecidoNaoMudaNadaENaoGeraStatus() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let porta = try subir(caixa: caixa, fonte: FonteMuda())
        let reg = Registro()
        servidor?.aoComandoRemoto = { c, pronto in reg.anotar(c); pronto() }
        let c = try conectar(porta)
        for j in [#"{"cmd":"zoom","valor":2}"#, #"{"cmd":"ev","valor":"alto"}"#, #"{"cmd":"lente"}"#, #"{"cmd":"trava_ae","valor":1}"#,
                  #"{"cmd":"qualidade","preset":"4k"}"#, #"não é json"#, #"{"cmd":"ev","valor":1,"valor":2}"#] {
            comandoRemoto(c, j)
        }
        // keyframe e ping depois dos ruins: provam que a conexão seguiu viva e processou tudo
        c.enviar(Pacote(tipo: 9, tempo: 5, dados: Data()).serializar())
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 10 } })
        XCTAssertTrue(reg.comandos.isEmpty, "\(reg.comandos)")
        XCTAssertTrue(status(c).isEmpty)
        XCTAssertEqual(caixa.valor, AjustesCamera())
    }

    func testSemQuemAplique() throws {
        let porta = try subir(caixa: CaixaAjustes(AjustesCamera()), fonte: FonteMuda())
        let c = try conectar(porta)
        comandoRemoto(c, #"{"cmd":"trava_ae","valor":true}"#)
        c.enviar(Pacote(tipo: 9, tempo: 5, dados: Data()).serializar())
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 10 } })
        XCTAssertTrue(status(c).isEmpty, "sem o app ligado ao servidor o comando é ignorado, sem status")
    }

    func testQualidadeRespeitaATravaPorCampoDoOBS() throws {
        // OBS fixou só o preset: o bitrate do comando passa, o preset é cortado
        let reg = Registro()
        let porta = try subir(caixa: CaixaAjustes(AjustesCamera()), fonte: FonteMuda())
        servidor?.aoComandoRemoto = { c, pronto in reg.anotar(c); pronto() }
        let c = try conectar(porta, #"{"v":1,"preset":"720p60"}"#)
        comandoRemoto(c, #"{"cmd":"qualidade","preset":"2160p30","bitrate":25000000}"#)
        XCTAssertTrue(c.esperar { reg.comandos.count == 1 })
        XCTAssertEqual(reg.comandos, [.qualidade(preset: nil, bitrate: 25_000_000)])
        comandoRemoto(c, #"{"cmd":"qualidade","preset":"1080p30"}"#)   // só o campo travado: ignora, sem status
        c.enviar(Pacote(tipo: 9, tempo: 5, dados: Data()).serializar())
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 10 } })
        XCTAssertEqual(reg.comandos.count, 1)
        XCTAssertEqual(status(c).count, 1, "só o status do comando que passou")
        servidor?.parar()

        // OBS fixou os dois: nada passa
        let reg2 = Registro()
        let porta2 = try subir(caixa: CaixaAjustes(AjustesCamera()), fonte: FonteMuda())
        servidor?.aoComandoRemoto = { c, pronto in reg2.anotar(c); pronto() }
        let c2 = try conectar(porta2, #"{"v":1,"preset":"1080p60","bitrate":8000000}"#)
        comandoRemoto(c2, #"{"cmd":"qualidade","preset":"720p30","bitrate":9000000}"#)
        c2.enviar(Pacote(tipo: 9, tempo: 5, dados: Data()).serializar())
        XCTAssertTrue(c2.esperarPacotes(0) { $0.contains { $0.tipo == 10 } })
        XCTAssertTrue(reg2.comandos.isEmpty)

        // nada travado: passa inteiro
        let reg3 = Registro()
        let porta3 = try subir(caixa: CaixaAjustes(AjustesCamera()), fonte: FonteMuda())
        servidor?.aoComandoRemoto = { c, pronto in reg3.anotar(c); pronto() }
        let c3 = try conectar(porta3)
        comandoRemoto(c3, #"{"cmd":"qualidade","preset":"1080p60","bitrate":9000000}"#)
        XCTAssertTrue(c3.esperar { reg3.comandos.count == 1 })
        XCTAssertEqual(reg3.comandos, [.qualidade(preset: .p1080_60, bitrate: 9_000_000)])
    }

    /// O status imediato não pode zerar a janela de fps: um segundo status logo depois repete a medida do ciclo.
    func testStatusImediatoNaoInventaFps() throws {
        let porta = try subir(caixa: CaixaAjustes(AjustesCamera()), fonte: FonteFalsa()) { $0.intervaloStatus = 0.3 }
        servidor?.aoComandoRemoto = { _, pronto in pronto() }
        let c = try conectar(porta)
        XCTAssertTrue(c.esperarPacotes(0) { $0.contains { $0.tipo == 3 } })
        let ciclo = try XCTUnwrap(status(c).first)
        comandoRemoto(c, #"{"cmd":"trava_af","valor":true}"#)
        XCTAssertTrue(c.esperarPacotes(0) { _ in self.status(c).count >= 2 })
        let imediato = status(c)[1]
        XCTAssertEqual(imediato["fps"] as? Double, ciclo["fps"] as? Double)
        XCTAssertEqual(imediato["kbps"] as? Int, ciclo["kbps"] as? Int)
    }

    func testNoArEKeyframeContinuamComoAntes() throws {
        let caixa = CaixaAjustes(AjustesCamera())
        let fonte = FonteFalsa()
        let porta = try subir(caixa: caixa, fonte: fonte)
        servidor?.aoComandoRemoto = { _, pronto in pronto() }
        let c = try conectar(porta)
        comandoRemoto(c, #"{"cmd":"noar","valor":true}"#)
        XCTAssertTrue(c.esperar { caixa.noAr })
        comandoRemoto(c, #"{"cmd":"keyframe"}"#)
        XCTAssertTrue(c.esperar { fonte.keyframesPedidos == 1 })
        comandoRemoto(c, #"{"cmd":"noar","valor":false}"#)
        XCTAssertTrue(c.esperar { !caixa.noAr })
        XCTAssertTrue(status(c).isEmpty, "noar e keyframe não geram status imediato")
    }
}

final class StatusRemotoCamposTests: XCTestCase {
    func testCamposNovosDoStatus() {
        var a = AjustesCamera()
        a.exposicaoTravada = true; a.brancoTravado = true
        let r = InfoRemota(lentes: [0.5, 1, 2, 5], fator: 5, evMin: -2, evMax: 2)
        let j = StatusCamera.campos(ajustes: a, leitura: LeituraCamera(), noAr: false, dim: DimensoesVideo(largura: 3840, altura: 2160, fps: 30),
                                    remoto: r, preset: .p2160_30)
        XCTAssertEqual(j["lentes"] as? [Double], [0.5, 1, 2, 5])
        XCTAssertEqual(j["fator"] as? Double, 5)
        XCTAssertEqual(j["ev_min"] as? Double, -2)
        XCTAssertEqual(j["ev_max"] as? Double, 2)
        XCTAssertEqual(j["preset"] as? String, "2160p30")
        XCTAssertEqual(j["trava_ae"] as? Bool, true)
        XCTAssertEqual(j["trava_af"] as? Bool, false)
        XCTAssertEqual(j["trava_bb"] as? Bool, true)
    }

    func testSemBotaoAcesoNaoMandaFator() {
        let j = StatusCamera.campos(ajustes: AjustesCamera(), leitura: LeituraCamera(), noAr: false, dim: nil,
                                    remoto: InfoRemota(lentes: [1, 2], fator: nil))
        XCTAssertNil(j["fator"])
        XCTAssertEqual(j["lentes"] as? [Double], [1, 2])
    }

    func testCamposAntigosContinuamIguais() {
        let j = StatusCamera.campos(ajustes: AjustesCamera(), leitura: LeituraCamera(), noAr: false, dim: nil)
        for k in ["lentes", "fator", "ev_min", "ev_max", "trava_ae", "trava_af", "trava_bb", "preset"] { XCTAssertNil(j[k], k) }
        XCTAssertNotNil(j["lente"]); XCTAssertNotNil(j["foco"])
    }
}
