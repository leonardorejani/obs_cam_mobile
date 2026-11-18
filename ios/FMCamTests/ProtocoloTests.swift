import XCTest
@testable import FMCam

final class ProtocoloTests: XCTestCase {
    func bytes(_ b: [UInt8]) -> Data { Data(b) }

    // Vetores dourados: o C++ testa os mesmos bytes.
    func testVetorDouradoQuadro() {
        let p = Pacote(tipo: 2, tempo: 0x0102030405060708, dados: bytes([0x01, 0, 0, 0, 1, 0x65]))
        XCTAssertEqual(p.serializar(), bytes([0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                              0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00, 0x01, 0x65]))
    }

    func testVetorDouradoPong() {
        let p = Pacote.pong(tempo: 1_000_000, eco: 123456)
        XCTAssertEqual(p.tipo, 10)
        XCTAssertEqual(p.serializar(), bytes([0x0A, 0, 0, 0, 0, 0, 0x0F, 0x42, 0x40,
                                              0, 0, 0, 8, 0, 0, 0, 0, 0, 0x01, 0xE2, 0x40]))
    }

    func testQuadroComFlags() {
        let c = Pacote.quadro(tempo: 5, chave: true, annexB: bytes([0, 0, 0, 1, 0x65]))
        XCTAssertEqual(c.dados.first, 0x01)
        let n = Pacote.quadro(tempo: 5, chave: false, annexB: bytes([0, 0, 0, 1, 0x41]))
        XCTAssertEqual(n.dados.first, 0x00)
    }

    func testParserIdaEVolta() throws {
        let p = Pacote(tipo: 2, tempo: 0xFFEEDDCCBBAA9988, dados: bytes([1, 2, 3]))
        var parser = ParserPacotes()
        XCTAssertEqual(try parser.alimentar(p.serializar()), [p])
    }

    func testParserPacoteCortadoByteAByte() throws {
        let a = Pacote(tipo: 2, tempo: 42, dados: bytes([9, 8, 7, 6, 5]))
        let b = Pacote(tipo: 9, tempo: 7, dados: Data())
        var parser = ParserPacotes()
        var saida: [Pacote] = []
        for byte in a.serializar() + b.serializar() {
            saida += try parser.alimentar(bytes([byte]))
        }
        XCTAssertEqual(saida, [a, b])
    }

    func testParserVariosPacotesNumaLeitura() throws {
        let a = Pacote(tipo: 9, tempo: 1, dados: Data())
        let b = Pacote(tipo: 4, tempo: 2, dados: Data("{\"cmd\":\"keyframe\"}".utf8))
        var parser = ParserPacotes()
        XCTAssertEqual(try parser.alimentar(a.serializar() + b.serializar()), [a, b])
    }

    func testTamanhoExcessivoEErro() {
        var cab = bytes([2, 0, 0, 0, 0, 0, 0, 0, 0])
        let tam = UInt32(Protocolo.tamanhoMaximoPacote + 1)
        cab.append(contentsOf: [UInt8(tam >> 24), UInt8((tam >> 16) & 0xFF), UInt8((tam >> 8) & 0xFF), UInt8(tam & 0xFF)])
        var parser = ParserPacotes()
        XCTAssertThrowsError(try parser.alimentar(cab)) {
            XCTAssertEqual($0 as? ErroProtocolo, .tamanhoExcessivo(Protocolo.tamanhoMaximoPacote + 1))
        }
    }

    func testTamanhoMaximoExatoNaoEErro() throws {
        var cab = bytes([2, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x80, 0x00, 0x00])
        XCTAssertEqual(cab.count, 13)
        var parser = ParserPacotes()
        XCTAssertEqual(try parser.alimentar(cab), [])
        cab.removeAll()
    }

    func testTipoDesconhecidoEPulado() throws {
        let desconhecido = Pacote(tipo: 77, tempo: 1, dados: bytes([1, 2, 3, 4])).serializar()
        let bom = Pacote(tipo: 9, tempo: 5, dados: Data())
        var parser = ParserPacotes()
        // pulado mesmo chegando aos pedaços, com o corpo cortado no meio
        var saida = try parser.alimentar(desconhecido.prefix(15))
        XCTAssertEqual(saida, [])
        saida = try parser.alimentar(desconhecido.dropFirst(15) + bom.serializar())
        XCTAssertEqual(saida, [bom])
    }

    // MARK: handshake

    func linha(_ s: String) -> Data { Data(s.utf8) }

    func testHandshakeValidoCompleto() {
        let r = Handshake.interpretar(linha: linha(#"{"v":1,"cliente":"obs-plugin 0.1","preset":"720p60","lente":"ultra","bitrate":5000000,"extra":1}"#))
        XCTAssertEqual(r, .aceito(PedidoHandshake(v: 1, cliente: "obs-plugin 0.1", preset: .p720_60, lente: .ultra, bitrate: 5_000_000)))
    }

    func testHandshakeMinimoUsaPadroes() {
        let r = Handshake.interpretar(linha: linha(#"{"v":1}"#))
        XCTAssertEqual(r, .aceito(PedidoHandshake(v: 1, cliente: "", preset: .p1080_30, lente: .wide, bitrate: 10_000_000)))
    }

    func testHandshakeValoresInvalidosCaemNoPadrao() {
        let r = Handshake.interpretar(linha: linha(#"{"v":1,"preset":"4k","lente":"x","bitrate":99}"#))
        XCTAssertEqual(r, .aceito(PedidoHandshake(v: 1, cliente: "", preset: .p1080_30, lente: .wide, bitrate: 10_000_000)))
        let r2 = Handshake.interpretar(linha: linha(#"{"v":1,"bitrate":"muito"}"#))
        XCTAssertEqual(r2, .aceito(PedidoHandshake(v: 1, cliente: "", preset: .p1080_30, lente: .wide, bitrate: 10_000_000)))
    }

    func testHandshakeVersaoErrada() {
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":2}"#)), .versaoNaoSuportada(2))
    }

    func testHandshakeInvalido() {
        XCTAssertEqual(Handshake.interpretar(linha: linha("{quebrado")), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"cliente":"x"}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"[1]"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":true}"#)), .invalido)
        XCTAssertEqual(Handshake.interpretar(linha: linha(#"{"v":"1"}"#)), .invalido)
    }

    func testHandshakeLinhaLonga() {
        let longa = #"{"v":1,"cliente":""# + String(repeating: "a", count: 5000) + #""}"#
        XCTAssertEqual(Handshake.interpretar(linha: linha(longa)), .invalido)
    }

    func testRespostas() throws {
        let ok = Handshake.respostaOk(aparelho: "iPhone14,5", nome: "Camera 1", id: "ABC", largura: 1920, altura: 1080, fps: 30)
        XCTAssertEqual(ok.last, 0x0A)
        let o = try XCTUnwrap(JSONSerialization.jsonObject(with: ok.dropLast()) as? [String: Any])
        XCTAssertEqual(o["ok"] as? Bool, true)
        XCTAssertEqual(o["nome"] as? String, "Camera 1")
        XCTAssertEqual(o["id"] as? String, "ABC")
        XCTAssertEqual(o["aparelho"] as? String, "iPhone14,5")
        XCTAssertEqual(o["w"] as? Int, 1920)
        XCTAssertEqual(o["codec"] as? String, "h264")
        let v = try XCTUnwrap(JSONSerialization.jsonObject(with: Handshake.respostaErro("versao").dropLast()) as? [String: Any])
        XCTAssertEqual(v["erro"] as? String, "versao")
        XCTAssertEqual(v["v_suportadas"] as? [Int], [1])
        XCTAssertEqual(v["ok"] as? Bool, false)
    }

    // MARK: AVCC -> Annex B

    func testAvccParaAnnexBDoisNalus() {
        let avcc = bytes([0, 0, 0, 2, 0xAA, 0xBB, 0, 0, 0, 3, 0xCC, 0xDD, 0xEE])
        XCTAssertEqual(AnnexB.converter(avcc: avcc), bytes([0, 0, 0, 1, 0xAA, 0xBB, 0, 0, 0, 1, 0xCC, 0xDD, 0xEE]))
    }

    func testAvccTruncadoERejeitado() {
        XCTAssertNil(AnnexB.converter(avcc: bytes([0, 0, 0, 5, 1, 2])))
        XCTAssertNil(AnnexB.converter(avcc: bytes([0, 0])))
    }
}
