import CoreGraphics
import XCTest
@testable import FMCam

final class CenaMudouTests: XCTestCase {
    private let agora = Date(timeIntervalSince1970: 1_800_000_000)

    func testSemPontoTocadoNaoFazNada() {
        XCTAssertFalse(CenaMudou.devolver(pontoTocado: nil, ultimaVez: nil, agora: agora))
    }

    func testPontoNoCentroNaoFazNada() {
        XCTAssertFalse(CenaMudou.devolver(pontoTocado: CGPoint(x: 0.5, y: 0.5), ultimaVez: nil, agora: agora))
        XCTAssertFalse(CenaMudou.devolver(pontoTocado: CGPoint(x: 0.51, y: 0.49), ultimaVez: nil, agora: agora))
    }

    func testPontoForaDoCentroVoltaUmaVez() {
        let p = CGPoint(x: 0.2, y: 0.7)
        XCTAssertTrue(CenaMudou.devolver(pontoTocado: p, ultimaVez: nil, agora: agora))
        // depois de devolver quem chama limpa o ponto: os eventos seguintes não fazem nada
        XCTAssertFalse(CenaMudou.devolver(pontoTocado: nil, ultimaVez: agora, agora: agora.addingTimeInterval(5)))
    }

    func testDebounceDeDoisSegundos() {
        let p = CGPoint(x: 0.8, y: 0.3)
        XCTAssertFalse(CenaMudou.devolver(pontoTocado: p, ultimaVez: agora, agora: agora.addingTimeInterval(1.9)))
        XCTAssertTrue(CenaMudou.devolver(pontoTocado: p, ultimaVez: agora, agora: agora.addingTimeInterval(2)))
    }
}
