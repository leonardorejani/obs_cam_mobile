import AVFoundation
import Foundation
import UIKit
import os

/// Orientação do vídeo enviado. O ângulo é em graus, sentido horário, contado a partir da posição nativa do sensor
/// (paisagem com o botão início à direita): 0 paisagem, 90 em pé, 180 paisagem para o outro lado, 270 em pé de cabeça
/// para baixo. É o mesmo ângulo de `AVCaptureConnection.videoRotationAngle`.
enum OrientacaoVideo {
    static let angulos = [0, 90, 180, 270]

    /// Posição física do aparelho para o ângulo do vídeo. nil para deitado de frente ou de costas e para desconhecido:
    /// nesses casos nada muda (a gravidade não diz para que lado o aparelho está).
    static func angulo(de o: UIDeviceOrientation) -> Int? {
        switch o {
        // UIDeviceOrientation.landscapeLeft é o botão início à direita, a posição nativa do sensor (e o inverso do
        // UIInterfaceOrientation de mesmo nome).
        case .landscapeLeft: return 0
        case .portrait: return 90
        case .landscapeRight: return 180
        case .portraitUpsideDown: return 270
        default: return nil
        }
    }

    /// Ângulo qualquer (o RotationCoordinator devolve múltiplos de 90, mas aceita lixo) para o múltiplo de 90 mais próximo.
    static func angulo(graus g: Double) -> Int {
        guard g.isFinite else { return 0 }
        let passos = Int((g / 90).rounded())
        return (((passos * 90) % 360) + 360) % 360
    }

    static func emPe(_ a: Int) -> Bool { a == 90 || a == 270 }

    /// Tamanho do vídeo depois de girar de `de` para `para`: troca largura e altura quando passa de deitado para em pé
    /// (ou o contrário). 0 para 180 não muda o tamanho, só o lado para cima.
    static func reorientar(_ d: DimensoesVideo, de: Int, para: Int) -> DimensoesVideo {
        guard emPe(de) != emPe(para) else { return d }
        return DimensoesVideo(largura: d.altura, altura: d.largura, fps: d.fps)
    }

    /// Tamanho de um formato de paisagem (1920x1080) quando o vídeo sai no ângulo dado (1080x1920 em pé).
    static func dimensoes(_ paisagem: DimensoesVideo, angulo: Int) -> DimensoesVideo {
        reorientar(paisagem, de: 0, para: angulo)
    }

    /// iOS 16, sem RotationCoordinator: orientação de vídeo equivalente.
    static func orientacaoDeVideo(_ a: Int) -> AVCaptureVideoOrientation {
        switch a {
        case 90: return .portrait
        case 180: return .landscapeLeft
        case 270: return .portraitUpsideDown
        default: return .landscapeRight
        }
    }

    /// Dimensão de um quadro de vídeo em pé (altura maior que a largura).
    static func quadroEmPe(largura: Int, altura: Int) -> Bool { altura > largura }
}

/// Segura a troca de orientação até ela ficar parada: o aparelho passa por posições intermediárias ao girar na mão
/// (e o acelerômetro treme), e cada troca real recria o codificador. Só vale uma orientação que durou `janela`.
struct DebounceOrientacao: Equatable {
    static let janela: TimeInterval = 0.5

    private(set) var aplicado: Int
    private var candidato: Int?
    private var desde = Date.distantPast

    init(inicial: Int = 0) { aplicado = inicial }

    /// Nova leitura da orientação física. nil (deitado, desconhecido) é ignorado: nem confirma nem cancela o candidato.
    mutating func entrada(_ a: Int?, agora: Date) {
        guard let a else { return }
        if a == aplicado {
            candidato = nil  // voltou para onde estava: a troca em andamento morre
        } else if a != candidato {
            candidato = a
            desde = agora
        }
    }

    /// O ângulo a aplicar agora, se o candidato já durou a janela inteira. Devolve uma vez por troca.
    mutating func pronto(agora: Date) -> Int? {
        guard let c = candidato, c != aplicado, agora.timeIntervalSince(desde) >= DebounceOrientacao.janela else { return nil }
        aplicado = c
        candidato = nil
        return c
    }

    /// Há uma troca esperando a janela fechar.
    var pendente: Bool { candidato != nil }
}

/// Lê a posição física do aparelho e diz o ângulo do vídeo. iOS 17: `AVCaptureDevice.RotationCoordinator` (acelerômetro,
/// já ignora o aparelho deitado). iOS 16: notificações de orientação do UIDevice. As duas fontes passam pelo mesmo
/// debounce. `aoFisico` vê cada mudança na hora (o preview acompanha na mesma hora), `aoAplicar` só depois do debounce
/// (o que sai para o OBS). Tudo roda na fila dada.
final class MonitorOrientacao {
    var aoFisico: ((Int) -> Void)?
    var aoAplicar: ((Int) -> Void)?

    private let fila: DispatchQueue
    private let trava = NSLock()
    private var debounce = DebounceOrientacao()
    private var fisico = 0
    private var observadorUIDevice: NSObjectProtocol?
    private var coordenador: AnyObject?   // AVCaptureDevice.RotationCoordinator
    private var observacao: NSKeyValueObservation?
    private var dispositivo: AVCaptureDevice?
    private var iniciado = false

    init(fila: DispatchQueue) { self.fila = fila }

    deinit {
        if let o = observadorUIDevice { NotificationCenter.default.removeObserver(o) }
    }

    /// Ângulo em uso no vídeo enviado (já passou pelo debounce).
    var anguloAplicado: Int { trava.lock(); defer { trava.unlock() }; return debounce.aplicado }
    /// Ângulo da posição física agora (sem debounce).
    var anguloFisico: Int { trava.lock(); defer { trava.unlock() }; return fisico }

    func iniciar() {
        trava.lock()
        if iniciado { trava.unlock(); return }
        iniciado = true
        trava.unlock()
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        observadorUIDevice = NotificationCenter.default.addObserver(forName: UIDevice.orientationDidChangeNotification, object: nil, queue: nil) { [weak self] _ in
            guard let self, !self.temCoordenador else { return }  // com o RotationCoordinator ligado ele manda
            let a = OrientacaoVideo.angulo(de: UIDevice.current.orientation)
            self.fila.async { self.entrada(a) }
        }
        // posição de partida: vale na hora, sem debounce (a câmera nasce já no lado certo)
        if let a = OrientacaoVideo.angulo(de: UIDevice.current.orientation) { estabelecer(a) }
    }

    private var temCoordenador: Bool { trava.lock(); defer { trava.unlock() }; return coordenador != nil }

    /// A câmera em uso mudou (troca de lente física, reconfiguração): o coordenador é do dispositivo.
    func usar(dispositivo d: AVCaptureDevice) {
        trava.lock()
        let mesmo = dispositivo === d
        dispositivo = d
        trava.unlock()
        guard !mesmo else { return }
        if #available(iOS 17.0, *) {
            let c = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: nil)
            observacao = c.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] cc, _ in
                let a = OrientacaoVideo.angulo(graus: Double(cc.videoRotationAngleForHorizonLevelCapture))
                self?.fila.async { self?.entrada(a) }
            }
            trava.lock()
            coordenador = c
            trava.unlock()
            let a = OrientacaoVideo.angulo(graus: Double(c.videoRotationAngleForHorizonLevelCapture))
            fila.async { if self.jaEstabelecido { self.entrada(a) } else { self.estabelecer(a) } }
        }
    }

    private var estabelecido = false
    private var jaEstabelecido: Bool { trava.lock(); defer { trava.unlock() }; return estabelecido }

    /// Primeira leitura da posição: vira o ponto de partida sem esperar o debounce.
    private func estabelecer(_ a: Int) {
        trava.lock()
        let antes = debounce.aplicado
        debounce = DebounceOrientacao(inicial: a)
        fisico = a
        estabelecido = true
        trava.unlock()
        aoFisico?(a)
        if a != antes { aoAplicar?(a) }
    }

    /// Só diagnóstico (Depuracao): finge que o aparelho girou para `a`.
    func forcar(_ a: Int) { fila.async { self.entrada(a) } }

    /// Na fila. `a` nil = deitado ou desconhecido: ignora.
    func entrada(_ a: Int?) {
        guard let a else { return }
        trava.lock()
        let mudouFisico = fisico != a
        fisico = a
        debounce.entrada(a, agora: Date())
        let esperar = debounce.pendente
        trava.unlock()
        if mudouFisico { aoFisico?(a) }
        if esperar {
            fila.asyncAfter(deadline: .now() + DebounceOrientacao.janela + 0.05) { [weak self] in self?.verificar() }
        }
    }

    private func verificar() {
        trava.lock()
        let novo = debounce.pronto(agora: Date())
        trava.unlock()
        if let novo {
            Log.captura.notice("fmcam orientação: vídeo gira para \(novo, privacy: .public) graus")
            aoAplicar?(novo)
        }
    }
}
