import CoreMedia
import CoreVideo
import XCTest
@testable import FMCam

final class CodificadorTests: XCTestCase {
    func testCodificaQuadrosSinteticos() throws {
        let cod = try Codificador(largura: 640, altura: 360, fps: 30, bitrate: 2_000_000)
        let trava = NSLock()
        var eventos: [EventoVideo] = []
        cod.aoEvento = { trava.lock(); eventos.append($0); trava.unlock() }
        for i in 0..<10 {
            var pb: CVPixelBuffer?
            CVPixelBufferCreate(nil, 640, 360, kCVPixelFormatType_32BGRA, nil, &pb)
            let buf = try XCTUnwrap(pb)
            CVPixelBufferLockBaseAddress(buf, [])
            memset(CVPixelBufferGetBaseAddress(buf), Int32(i * 20), CVPixelBufferGetDataSize(buf))
            CVPixelBufferUnlockBaseAddress(buf, [])
            cod.codificar(pixelBuffer: buf, pts: CMTime(value: Int64(i), timescale: 30))
        }
        cod.finalizar()
        trava.lock(); let evs = eventos; trava.unlock()

        guard case .config(_, let params)? = evs.first else { return XCTFail("primeiro evento deve ser config: \(evs.count)") }
        XCTAssertEqual(Array(params.prefix(4)), [0, 0, 0, 1])
        guard params.count > 4 else { return XCTFail("config curta") }
        XCTAssertEqual(params[4] & 0x1F, 7)  // SPS (nal_ref_idc varia)
        let quadros = evs.compactMap { e -> (Bool, Data)? in if case .quadro(_, let c, let d) = e { return (c, d) }; return nil }
        XCTAssertEqual(quadros.count, 10)
        guard let primeiro = quadros.first else { return XCTFail("sem quadros") }
        XCTAssertTrue(primeiro.0, "primeiro quadro é chave")
        for q in quadros { XCTAssertEqual(Array(q.1.prefix(4)), [0, 0, 0, 1]) }
    }
}

final class CodificadorGrandeTests: XCTestCase {
    func testQuaisTamanhosSaoGrandes() {
        XCTAssertFalse(Codificador.ehGrande(largura: 1920, altura: 1080))
        XCTAssertFalse(Codificador.ehGrande(largura: 1080, altura: 1920), "1080p em pé tem o mesmo número de pixels")
        XCTAssertFalse(Codificador.ehGrande(largura: 1280, altura: 720))
        XCTAssertTrue(Codificador.ehGrande(largura: 3840, altura: 2160))
        XCTAssertTrue(Codificador.ehGrande(largura: 2160, altura: 3840), "4K em pé")
    }

    /// O ajuste de baixa latência do 4K não pode impedir o codificador de nascer nem de entregar config e quadro-chave.
    func testCodificador4KNasceEEntregaConfigEChave() throws {
        let cod = try Codificador(largura: 3840, altura: 2160, fps: 30, bitrate: 25_000_000)
        let col = FonteCameraTests.Coletor()
        cod.aoEvento = col.adicionar
        for i in 0..<3 {
            var pb: CVPixelBuffer?
            CVPixelBufferCreate(nil, 3840, 2160, kCVPixelFormatType_32BGRA, nil, &pb)
            cod.codificar(pixelBuffer: try XCTUnwrap(pb), pts: CMTime(value: Int64(i), timescale: 30))
        }
        cod.finalizar()
        XCTAssertEqual(col.configs, 1)
        XCTAssertEqual(col.quadros.count, 3)
        XCTAssertTrue(col.quadros.first?.0 ?? false)
    }
}
