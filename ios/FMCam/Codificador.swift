import CoreMedia
import Foundation
import os
import VideoToolbox

/// AVCC (prefixo de tamanho) para Annex B (`00 00 00 01`).
enum AnnexB {
    static let inicio: [UInt8] = [0, 0, 0, 1]

    /// nil se o buffer estiver truncado ou inconsistente.
    static func converter(avcc: Data, tamanhoPrefixo: Int = 4) -> Data? {
        guard (1...4).contains(tamanhoPrefixo) else { return nil }
        let b = [UInt8](avcc)
        var saida = Data(capacity: b.count + 16)
        var i = 0
        while i < b.count {
            guard i + tamanhoPrefixo <= b.count else { return nil }
            var n = 0
            for k in 0..<tamanhoPrefixo { n = (n << 8) | Int(b[i + k]) }
            i += tamanhoPrefixo
            guard n > 0, i + n <= b.count else { return nil }
            saida.append(contentsOf: inicio)
            saida.append(contentsOf: b[i..<(i + n)])
            i += n
        }
        return saida
    }

    static func parametros(de fd: CMFormatDescription) -> (dados: Data, prefixo: Int)? {
        var quantidade = 0
        var prefixo: Int32 = 0
        var status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &quantidade, nalUnitHeaderLengthOut: &prefixo)
        guard status == noErr, quantidade > 0 else { return nil }
        var saida = Data()
        for i in 0..<quantidade {
            var ptr: UnsafePointer<UInt8>?
            var tam = 0
            status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fd, parameterSetIndex: i, parameterSetPointerOut: &ptr, parameterSetSizeOut: &tam,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
            guard status == noErr, let ptr else { return nil }
            saida.append(contentsOf: inicio)
            saida.append(ptr, count: tam)
        }
        return (saida, Int(prefixo))
    }
}

/// H.264 em tempo real, sem B-frames.
final class Codificador {
    var aoEvento: ((EventoVideo) -> Void)?

    let largura: Int
    let altura: Int
    let fps: Int
    private var sessao: VTCompressionSession?
    private let trava = NSLock()
    private var forcarChave = true
    private var ultimaConfig: Data?

    init(largura: Int, altura: Int, fps: Int, bitrate: Int) throws {
        self.largura = largura
        self.altura = altura
        self.fps = fps
        var s: VTCompressionSession?
        let st = VTCompressionSessionCreate(
            allocator: nil, width: Int32(largura), height: Int32(altura),
            codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard st == noErr, let s else {
            Log.codificador.notice("fmcam codificador: VTCompressionSessionCreate falhou: \(st, privacy: .public)")
            throw ErroFonte.camera("Não foi possível criar o codificador de vídeo.") }
        func ajustar(_ chave: CFString, _ valor: CFTypeRef) { VTSessionSetProperty(s, key: chave, value: valor) }
        ajustar(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        ajustar(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        ajustar(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        ajustar(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2))
        ajustar(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 2))
        ajustar(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        ajustar(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        if Codificador.ehGrande(largura: largura, altura: altura) {
            // Acima de 1080p cada quadro leva mais para codificar e o codificador aceita segurar quadros para trabalhar
            // melhor. Tempo real: nenhum quadro de espera, e velocidade antes de qualidade de compressão.
            // Propriedade que o aparelho não conhece só devolve erro (VTSessionSetProperty), não derruba a criação.
            ajustar(kVTCompressionPropertyKey_MaxFrameDelayCount, NSNumber(value: 0))
            ajustar(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        }
        VTCompressionSessionPrepareToEncodeFrames(s)
        sessao = s
    }

    /// Maior que 1080p (o 4K): único tamanho que ganha o ajuste de baixa latência acima.
    static func ehGrande(largura: Int, altura: Int) -> Bool { largura * altura > 1920 * 1080 }

    /// Troca de lente: reenvia SPS/PPS (tipo 1) antes do próximo quadro, que sai como chave.
    func recomecarConfigEChave() {
        trava.lock(); forcarChave = true; ultimaConfig = nil; trava.unlock()
    }

    func pedirKeyframe() {
        trava.lock(); forcarChave = true; trava.unlock()
    }

    /// Muda o bitrate médio sem recriar o codificador (não muda o tamanho, então não há tipo 1 novo).
    func ajustarBitrate(_ bps: Int) {
        trava.lock(); let s = sessao; trava.unlock()
        guard let s else { return }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bps))
    }

    func codificar(_ amostra: CMSampleBuffer) {
        guard let pb = CMSampleBufferGetImageBuffer(amostra) else { return }
        codificar(pixelBuffer: pb, pts: CMSampleBufferGetPresentationTimeStamp(amostra),
                  duracao: CMSampleBufferGetDuration(amostra))
    }

    func codificar(pixelBuffer: CVPixelBuffer, pts: CMTime, duracao: CMTime = .invalid) {
        trava.lock()
        let s = sessao
        let forcar = forcarChave
        forcarChave = false
        trava.unlock()
        guard let s else { return }
        // Quadro que ainda saiu da câmera no tamanho de antes de um giro: o codificador é de outro tamanho, descarta.
        guard CVPixelBufferGetWidth(pixelBuffer) == largura, CVPixelBufferGetHeight(pixelBuffer) == altura else {
            trava.lock(); forcarChave = forcarChave || forcar; trava.unlock()
            return
        }
        let props: CFDictionary? = forcar ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        let st = VTCompressionSessionEncodeFrame(
            s, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: duracao,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, flags, saida in
            if status != noErr {
                Log.codificador.notice("fmcam codificador: erro na saída do codificador: \(status, privacy: .public)")
                return
            }
            if flags.contains(.frameDropped) { Log.codificador.notice("fmcam codificador: codificador descartou um quadro") }
            guard let saida else { return }
            self?.tratarSaida(saida)
        }
        if st != noErr { Log.codificador.notice("fmcam codificador: VTCompressionSessionEncodeFrame falhou: \(st, privacy: .public)") }
    }

    private func tratarSaida(_ sb: CMSampleBuffer) {
        guard CMSampleBufferDataIsReady(sb), let fd = CMSampleBufferGetFormatDescription(sb),
              let bloco = CMSampleBufferGetDataBuffer(sb),
              let (params, prefixo) = AnnexB.parametros(de: fd) else { return }
        let tempo = micros(CMSampleBufferGetPresentationTimeStamp(sb))
        let anexos = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]]
        let chave = !((anexos?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)

        var avcc = Data(count: CMBlockBufferGetDataLength(bloco))
        let st = avcc.withUnsafeMutableBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(bloco, atOffset: 0, dataLength: raw.count, destination: base)
        }
        guard st == kCMBlockBufferNoErr, let annexB = AnnexB.converter(avcc: avcc, tamanhoPrefixo: prefixo) else { return }

        trava.lock()
        let mudou = params != ultimaConfig
        ultimaConfig = params
        trava.unlock()
        if mudou { aoEvento?(.config(tempo: tempo, dados: params)) }
        aoEvento?(.quadro(tempo: tempo, chave: chave, dados: annexB))
    }

    /// Entrega o que o codificador ainda segura, sem encerrar a sessão.
    func esvaziar() {
        trava.lock(); let s = sessao; trava.unlock()
        if let s { VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid) }
    }

    /// Termina os quadros pendentes (entregando-os) e encerra a sessão.
    func finalizar() {
        trava.lock(); let s = sessao; sessao = nil; trava.unlock()
        guard let s else { return }
        VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(s)
    }

    deinit { finalizar() }
}
