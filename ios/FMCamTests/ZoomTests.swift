import XCTest
@testable import FMCam

final class ZoomTests: XCTestCase {
    // Tripla: ultra é o fator 1, a grande-angular entra em 2 e a tele em 6 (fatores injetados, nada fixo no app).
    private let tripla = MapaZoom(constituintes: [.tele, .ultra, .wide], trocas: [2, 6])
    private let dupla = MapaZoom(constituintes: [.ultra, .wide], trocas: [2])

    func testFatoresDaTripla() {
        XCTAssertEqual(tripla.fatorBase(.ultra), 1)
        XCTAssertEqual(tripla.fatorBase(.wide), 2)
        XCTAssertEqual(tripla.fatorBase(.tele), 6)
        XCTAssertEqual(tripla.fator(lente: .wide, zoom: 2), 4, "o 2x é a grande-angular com zoom 2")
        XCTAssertEqual(tripla.fator(lente: .wide, zoom: 0.5), 2, "zoom abaixo de 1 não existe")
    }

    func testOutroAparelhoUsaOsProprios() {
        let promax = MapaZoom(constituintes: [.ultra, .wide, .tele], trocas: [2, 10])
        XCTAssertEqual(promax.fatorBase(.tele), 10)
        XCTAssertEqual(promax.fator(lente: .tele, zoom: 1), 10)
        XCTAssertEqual(promax.fatorBase(.wide), 2)
    }

    func testDuplaNaoTemTele() {
        XCTAssertEqual(dupla.fatorBase(.wide), 2)
        XCTAssertNil(dupla.fatorBase(.tele))
        XCTAssertNil(dupla.fator(lente: .tele, zoom: 1))
    }

    func testFaltaPontoDeTrocaDaNil() {
        let quebrado = MapaZoom(constituintes: [.ultra, .wide, .tele], trocas: [2])
        XCTAssertNil(quebrado.fatorBase(.tele))
    }

    func testTaxaParaQuartoDeSegundo() throws {
        XCTAssertEqual(RampaZoom.duracaoPadrao, 0.25)
        // de 1 para 4 são 2 potências de dois: em 0,25 s a taxa é 8 por segundo
        XCTAssertEqual(try XCTUnwrap(RampaZoom.taxa(de: 1, para: 4)), 8, accuracy: 0.001)
        // a duração é a mesma em qualquer distância e nos dois sentidos
        let ida = try XCTUnwrap(RampaZoom.taxa(de: 2, para: 10))
        let volta = try XCTUnwrap(RampaZoom.taxa(de: 10, para: 2))
        XCTAssertEqual(ida, volta, accuracy: 0.0001)
        XCTAssertEqual(log2(5.0) / Double(ida), 0.25, accuracy: 0.001)
    }

    func testTaxaSemDistanciaOuEntradaInvalida() {
        XCTAssertNil(RampaZoom.taxa(de: 2, para: 2))
        XCTAssertNil(RampaZoom.taxa(de: 0, para: 2))
        XCTAssertNil(RampaZoom.taxa(de: 2, para: -1))
        XCTAssertNil(RampaZoom.taxa(de: 1, para: 2, duracao: 0))
    }

    func testRegraDeFallbackDeFormato() {
        XCTAssertEqual(ModoCamera.escolher(virtualExiste: true, virtualTemFormato: true, estabilizar: false, virtualEstabiliza: false), .virtual)
        XCTAssertEqual(ModoCamera.escolher(virtualExiste: true, virtualTemFormato: true, estabilizar: true, virtualEstabiliza: true), .virtual)
        XCTAssertEqual(ModoCamera.escolher(virtualExiste: false, virtualTemFormato: false, estabilizar: false, virtualEstabiliza: false),
                       .fisica(motivo: "sem dispositivo virtual"))
        XCTAssertEqual(ModoCamera.escolher(virtualExiste: true, virtualTemFormato: false, estabilizar: false, virtualEstabiliza: false),
                       .fisica(motivo: "formato não existe no dispositivo virtual"))
        if case .fisica = ModoCamera.escolher(virtualExiste: true, virtualTemFormato: true, estabilizar: true, virtualEstabiliza: false) {} else {
            XCTFail("estabilização ligada sem suporte no virtual cai para a física")
        }
    }
}
