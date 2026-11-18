import XCTest
@testable import FMCam

final class NomeCameraTests: XCTestCase {
    func novoDefaults() -> UserDefaults {
        let nome = "teste-nome-\(UUID().uuidString)"
        let d = UserDefaults(suiteName: nome)!
        d.removePersistentDomain(forName: nome)
        return d
    }
    let id = "6f1c2a3b-0000-0000-0000-000000000000"

    func testPadraoUsaQuatroPrimeirosDoId() {
        XCTAssertEqual(NomeCamera.padrao(idVendor: id), "iPhone 6F1C")
        XCTAssertEqual(NomeCamera.padrao(idVendor: ""), "iPhone")
        XCTAssertNotEqual(NomeCamera.padrao(idVendor: "AAAA-1"), NomeCamera.padrao(idVendor: "BBBB-1"))
    }

    func testSemSalvoDevolvePadrao() {
        XCTAssertEqual(NomeCamera.ler(defaults: novoDefaults(), idVendor: id), "iPhone 6F1C")
    }

    func testPersistenciaEntreLeituras() {
        let d = novoDefaults()
        XCTAssertEqual(NomeCamera.salvar("  Palco 1 ", defaults: d, idVendor: id), "Palco 1")
        XCTAssertEqual(NomeCamera.ler(defaults: d, idVendor: id), "Palco 1")
        XCTAssertEqual(d.string(forKey: NomeCamera.chave), "Palco 1")
    }

    func testNomeVazioVoltaAoPadrao() {
        let d = novoDefaults()
        NomeCamera.salvar("Palco 1", defaults: d, idVendor: id)
        XCTAssertEqual(NomeCamera.salvar("   ", defaults: d, idVendor: id), "iPhone 6F1C")
        XCTAssertNil(d.string(forKey: NomeCamera.chave))
    }

    func testNomeLongoELimitado() {
        let d = novoDefaults()
        let r = NomeCamera.salvar(String(repeating: "a", count: 100), defaults: d, idVendor: id)
        XCTAssertEqual(r.count, NomeCamera.tamanhoMaximo)
    }
}
