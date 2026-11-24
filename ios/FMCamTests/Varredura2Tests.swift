import XCTest
@testable import FMCam

/// Defeitos achados na varredura 2 (docs/varreduras/varredura-2.md).
final class Varredura2Tests: XCTestCase {
    /// Troca de qualidade na mesma câmera mantém entrada e saída: só o formato ativo muda (antes a sessão tirava e punha
    /// a entrada de volta e o vídeo parava 1 a 1,6 s no 15 Pro Max).
    func testMesmaCameraSoTrocaOFormato() {
        XCTAssertTrue(ReusoDaSessao.mantemEntrada(mesmoDispositivo: true, entradas: 1, saidaAtualNaSessao: true))
    }

    func testOutraCameraRefazEntradaESaida() {
        XCTAssertFalse(ReusoDaSessao.mantemEntrada(mesmoDispositivo: false, entradas: 1, saidaAtualNaSessao: true))
        XCTAssertFalse(ReusoDaSessao.mantemEntrada(mesmoDispositivo: true, entradas: 0, saidaAtualNaSessao: true))
        XCTAssertFalse(ReusoDaSessao.mantemEntrada(mesmoDispositivo: true, entradas: 1, saidaAtualNaSessao: false))
    }

    /// A câmera sem permissão, sem câmera, pausada pelo iOS ou com erro: antes o estado era calculado e nunca aparecia (tela
    /// preta com "Esperando"). Agora vira aviso no meio da tela.
    func testEstadoDaCameraApareceNaTela() {
        XCTAssertNil(Rotulos.avisoCamera(.ligada))
        XCTAssertNil(Rotulos.avisoCamera(.ligando))
        XCTAssertNil(Rotulos.avisoCamera(.desligada))
        XCTAssertNotNil(Rotulos.avisoCamera(.semPermissao))
        XCTAssertNotNil(Rotulos.avisoCamera(.semCamera))
        XCTAssertNotNil(Rotulos.avisoCamera(.interrompida))
        XCTAssertEqual(Rotulos.avisoCamera(.erro("A câmera parou: x")), "A câmera parou: x")
        for e in [EstadoCaptura.semPermissao, .semCamera, .interrompida] {
            let t = Rotulos.avisoCamera(e) ?? ""
            XCTAssertFalse(t.contains("\u{2014}") || t.contains("\u{2013}") || t.contains(";"), "texto sem travessão nem ponto e vírgula")
        }
    }

    /// Branco travado sem temperatura escolhida trava no valor de agora: o painel dizia "5500 K", que não era verdade.
    func testBrancoTravadoSemTemperaturaNaoInventaNumero() {
        var a = AjustesCamera()
        a.brancoTravado = true
        XCTAssertEqual(Rotulos.temperatura(a), "Atual")
        a.kelvin = 4300
        XCTAssertEqual(Rotulos.temperatura(a), "4300 K")
    }
}
