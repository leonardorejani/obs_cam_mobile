import AVFoundation
import CoreMedia
import Foundation
import os

struct ConfigVideo: Equatable {
    var preset: Preset
    var lente: Lente
    var bitrate: Int
    /// O cliente pediu uma lente de propósito. Se não pediu, a lente escolhida no iPhone fica.
    var lenteInformada: Bool

    init(preset: Preset = .p1080_30, lente: Lente? = nil, bitrate: Int? = nil) {
        self.preset = preset
        self.lente = lente ?? .wide
        self.lenteInformada = lente != nil
        self.bitrate = bitrate ?? preset.bitratePadrao
    }

    init(_ pedido: PedidoHandshake) {
        self.init(preset: pedido.preset, lente: pedido.lenteInformada ? pedido.lente : nil, bitrate: pedido.bitrate)
    }

    static func == (a: ConfigVideo, b: ConfigVideo) -> Bool {
        a.preset == b.preset && a.lente == b.lente && a.bitrate == b.bitrate
    }
}

struct DimensoesVideo: Equatable {
    var largura: Int
    var altura: Int
    var fps: Int
}

enum ErroFonte: Error, Equatable {
    case camera(String)

    var mensagem: String {
        switch self { case .camera(let m): return m }
    }
}

enum EventoVideo {
    /// SPS e PPS em Annex B, com o tempo do quadro que vem a seguir.
    case config(tempo: UInt64, dados: Data)
    case quadro(tempo: UInt64, chave: Bool, dados: Data)
    /// O tamanho do vídeo mudou no meio da conexão (o aparelho girou entre paisagem e em pé). Vem antes do tipo 1 novo.
    case dimensao(DimensoesVideo)
    /// Falha depois de iniciado (câmera tomada por outro app, por exemplo).
    case erro(String)
}

/// Quem produz vídeo já codificado. O app usa a câmera real, os testes uma fonte falsa.
protocol FonteDeVideo: AnyObject {
    var aoEvento: ((EventoVideo) -> Void)? { get set }
    /// Chama `completion` uma vez. Os eventos só começam depois do sucesso.
    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void)
    func parar()
    func pedirKeyframe()
    /// Troca de qualidade com a conexão aberta (só quando não foi o OBS que fixou). Chama `completion` uma vez.
    func reconfigurar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void)
}

extension FonteDeVideo {
    func reconfigurar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.failure(.camera("Troca de qualidade indisponível.")))
    }
}

func relogioHostMicros() -> UInt64 {
    micros(CMClockGetTime(CMClockGetHostTimeClock()))
}

func micros(_ t: CMTime) -> UInt64 {
    guard t.isValid else { return 0 }
    let v = CMTimeConvertScale(t, timescale: 1_000_000, method: .default).value
    return v < 0 ? 0 : UInt64(v)
}

/// Decide se uma partida de câmera pedida por um cliente ainda deve ligar a câmera.
enum ControleDePartida {
    /// Partida cancelada (o cliente saiu enquanto a autorização ou a configuração andava) só segue se o app
    /// está em primeiro plano, quando a câmera fica ligada de qualquer jeito para o preview.
    static func deveProsseguir(emPrimeiroPlano: Bool, cancelada: Bool) -> Bool { emPrimeiroPlano || !cancelada }

    /// O app chama "primeiro plano" duas vezes ao abrir (onAppear e a fase active, ~0,7 s de diferença no aparelho). A segunda
    /// não pode mexer na sessão que já está de pé nem na que um cliente acabou de pedir.
    static func deveLigarPreview(jaEmPrimeiroPlano: Bool, sessaoRodando: Bool, configurada: Bool) -> Bool {
        !(jaEmPrimeiroPlano && sessaoRodando && configurada)
    }
}

/// Reconfigurar a sessão para outro formato da MESMA câmera: só muda o formato ativo, sem tirar e pôr a entrada e a saída.
enum ReusoDaSessao {
    static func mantemEntrada(mesmoDispositivo: Bool, entradas: Int, saidaAtualNaSessao: Bool) -> Bool {
        mesmoDispositivo && entradas == 1 && saidaAtualNaSessao
    }
}

// MARK: - Auxiliares de câmera

enum Captura {
    /// Fila única para ligar, trocar e desligar câmera: nada disso roda na fila do servidor (startRunning bloqueia).
    static let filaSessao = DispatchQueue(label: "fmcam.captura.sessao")

    static func tipo(de lente: Lente) -> AVCaptureDevice.DeviceType {
        switch lente {
        case .wide: return .builtInWideAngleCamera
        case .ultra: return .builtInUltraWideCamera
        case .tele: return .builtInTelephotoCamera
        }
    }

    static func lente(de d: AVCaptureDevice) -> Lente? {
        switch d.deviceType {
        case .builtInWideAngleCamera: return .wide
        case .builtInUltraWideCamera: return .ultra
        case .builtInTelephotoCamera: return .tele
        default: return nil
        }
    }

    /// Lente pedida, com queda para a grande-angular se o aparelho não tiver.
    static func dispositivo(para lente: Lente) -> AVCaptureDevice? {
        if let d = AVCaptureDevice.default(tipo(de: lente), for: .video, position: .back) { return d }
        return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
    }

    /// Dispositivo virtual traseiro: tripla, senão dupla grande-angular. nil se o aparelho não tem (aí vale a câmera física).
    static func dispositivoVirtual() -> AVCaptureDevice? {
        for t in [AVCaptureDevice.DeviceType.builtInTripleCamera, .builtInDualWideCamera] {
            if let d = AVCaptureDevice.default(t, for: .video, position: .back) { return d }
        }
        return nil
    }

    /// Mesmo critério que a sessão usa para escolher o formato (prefere o que não é "binned").
    static func formato(_ d: AVCaptureDevice, _ p: Preset) -> AVCaptureDevice.Format? {
        let fs = d.formats.filter { f in
            let dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(dim.width) == p.largura && Int(dim.height) == p.altura
                && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(p.fps) && Double(p.fps) <= $0.maxFrameRate }
        }
        return fs.first(where: { !$0.isVideoBinned }) ?? fs.first
    }

    static func mapaZoom(_ d: AVCaptureDevice) -> MapaZoom {
        MapaZoom(constituintes: d.constituentDevices.compactMap { lente(de: $0) },
                 trocas: d.virtualDeviceSwitchOverVideoZoomFactors.map { $0.doubleValue })
    }

    /// Formatos que a lente atual aceita de verdade (tamanho e fps).
    static func presetsSuportados(lente: Lente) -> Set<Preset> {
        #if targetEnvironment(simulator)
        return [.p720_30, .p720_60, .p1080_30, .p1080_60]
        #else
        guard let d = dispositivo(para: lente) else { return [.p720_30, .p1080_30] }
        let v = dispositivoVirtual()
        return Set(Preset.allCases.filter { p in formato(d, p) != nil || v.flatMap { formato($0, p) } != nil })
        #endif
    }

    static func infosLentes() -> [InfoLente] {
        let ds = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInUltraWideCamera, .builtInWideAngleCamera, .builtInTelephotoCamera],
            mediaType: .video, position: .back)
        return ds.devices.compactMap { d in
            lente(de: d).map { InfoLente(lente: $0, fov: Double(d.activeFormat.videoFieldOfView)) }
        }
    }

    static func autorizar(_ resposta: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: resposta(true)
        case .notDetermined: AVCaptureDevice.requestAccess(for: .video, completionHandler: resposta)
        default: resposta(false)
        }
    }
}

/// Erro de execução da câmera: uma religada por janela de `intervalo`, não uma por vida do app. Uma segunda falha logo em
/// seguida não entra em laço, mas um erro horas depois de a câmera ter voltado a rodar ganha outra tentativa.
struct PoliticaDeReinicio: Equatable {
    static let intervalo: TimeInterval = 30
    private var ultima: Date?

    mutating func podeTentar(agora: Date) -> Bool {
        if let u = ultima, agora.timeIntervalSince(u) < Self.intervalo { return false }
        ultima = agora
        return true
    }

    /// A sessão foi ligada de novo pelo caminho normal: o erro seguinte volta a ter direito a uma tentativa.
    mutating func zerar() { ultima = nil }
}

enum EstadoCaptura: Equatable {
    case desligada, ligando, ligada, semCamera, semPermissao
    /// O iOS pausou a câmera com o app na frente (ligação, outro app usando a câmera, temperatura).
    case interrompida
    case erro(String)
}

/// O que a fonte de vídeo precisa da captura. A real é a `CapturaCompartilhada`, os testes usam uma falsa.
protocol MotorDeCaptura: AnyObject {
    /// Garante a câmera ligada no formato pedido (reconfigura se for outro) e devolve as dimensões.
    /// `cancelada` diz se quem pediu já desistiu (o cliente saiu): nesse caso a câmera não liga por causa dele.
    func prepararParaTransmitir(_ config: ConfigVideo, cancelada: @escaping () -> Bool,
                                completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void)
    /// Só quem tem consumidor recebe quadros: sem cliente ninguém codifica.
    func definirConsumidor(dono: AnyObject, amostra: @escaping (CMSampleBuffer) -> Void,
                           aoFalha: @escaping (String) -> Void, aoReconfigurar: @escaping (DimensoesVideo) -> Void)
    func liberarConsumidor(dono: AnyObject)
    /// Troca o formato (preset) com a câmera já ligada, sem derrubar nada.
    func mudarPreset(_ preset: Preset, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void)
}

extension MotorDeCaptura {
    func mudarPreset(_ preset: Preset, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        completion(.failure(.camera("Troca de formato indisponível.")))
    }
}

// MARK: - Captura compartilhada (uma sessão para preview e envio)

final class CapturaCompartilhada: NSObject, MotorDeCaptura, AVCaptureVideoDataOutputSampleBufferDelegate {
    private struct Consumidor {
        weak var dono: AnyObject?
        var amostra: (CMSampleBuffer) -> Void
        var aoFalha: (String) -> Void
        var aoReconfigurar: (DimensoesVideo) -> Void
    }

    /// Sessão que o preview da tela usa.
    let sessao = AVCaptureSession()
    /// Chamados na fila da câmera.
    var aoEstado: ((EstadoCaptura) -> Void)?
    /// Mudança de ajustes que nasceu aqui (pedido de lente do OBS, trava de foco, queda de lente).
    var aoMutacao: ((@escaping AjustesCamera.Mutacao) -> Void)?
    /// ISO e obturador de agora (a cada meio segundo, mesmo no automático).
    var aoLeitura: ((LeituraCamera) -> Void)?
    var aoLimites: ((LimitesCamera) -> Void)?
    var aoDimensao: ((DimensoesVideo) -> Void)?
    /// Aviso curto para a tela (ex.: estabilização indisponível).
    var aoAviso: ((String) -> Void)?

    /// Posição física do aparelho: o vídeo enviado e o preview giram com ela.
    let monitorOrientacao = MonitorOrientacao(fila: Captura.filaSessao)
    /// Posição física agora (sem debounce), para o preview e as guias. Chamado na fila da câmera.
    var aoAnguloFisico: ((Int) -> Void)?

    private var filaSessao: DispatchQueue { Captura.filaSessao }
    private let filaAmostras = DispatchQueue(label: "fmcam.captura.amostras", qos: .userInteractive)
    private let trava = NSLock()
    private var consumidor: Consumidor?
    private var logPrimeiroQuadro = true
    private var observadores: [NSObjectProtocol] = []

    // Só na fila da câmera:
    private var ajustes = AjustesCamera()
    private(set) var preset = Preset.p1080_30
    private var configurado: (preset: Preset, lente: Lente)?
    /// Tamanho do vídeo enviado, já no ângulo em uso (1080x1920 com o aparelho em pé).
    private var dimAtual: DimensoesVideo?
    /// Ângulo de rotação que está na conexão de saída agora.
    private var anguloConexao = 0
    private var dispositivoAtual: AVCaptureDevice?
    private var saidaAtual: AVCaptureVideoDataOutput?
    /// Usando o dispositivo virtual: a lente muda por rampa de zoom, sem reconfigurar a sessão.
    private var mapaVirtual: MapaZoom?
    private var obsConstituinte: NSKeyValueObservation?
    private var dispositivoObservado: AVCaptureDevice?
    /// Ponto em que o usuário tocou para focar (nil = foco no centro, nada a devolver).
    private var pontoTocado: CGPoint?
    private var ultimaDevolucao: Date?
    var reinicio = PoliticaDeReinicio()
    private var emPrimeiroPlano = false
    private var timerLeitura: DispatchSourceTimer?

    override init() {
        super.init()
        let t = DispatchSource.makeTimerSource(queue: Captura.filaSessao)
        t.schedule(deadline: .now() + 1, repeating: 0.5)
        t.setEventHandler { [weak self] in self?.lerCamera() }
        t.resume()
        timerLeitura = t
        monitorOrientacao.aoFisico = { [weak self] a in self?.aoAnguloFisico?(a) }
        monitorOrientacao.aoAplicar = { [weak self] a in self?.aplicarRotacao(a) }
        monitorOrientacao.iniciar()
        let c = NotificationCenter.default
        observadores.append(c.addObserver(forName: .AVCaptureSessionWasInterrupted, object: sessao, queue: nil) { [weak self] n in
            let r = (n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? NSNumber)?.intValue ?? -1
            Log.captura.notice("fmcam captura: sessão da câmera interrompida, motivo \(r, privacy: .public)")
            // Em segundo plano a câmera para de propósito. Os outros motivos acontecem com o app na frente e a tela mostra.
            guard r != AVCaptureSession.InterruptionReason.videoDeviceNotAvailableInBackground.rawValue else { return }
            self?.filaSessao.async { if self?.emPrimeiroPlano == true { self?.aoEstado?(.interrompida) } }
        })
        observadores.append(c.addObserver(forName: .AVCaptureSessionInterruptionEnded, object: sessao, queue: nil) { [weak self] _ in
            Log.captura.notice("fmcam captura: interrupção da câmera terminou")
            self?.filaSessao.async { if self?.emPrimeiroPlano == true, self?.sessao.isRunning == true { self?.aoEstado?(.ligada) } }
        })
        observadores.append(c.addObserver(forName: .AVCaptureDeviceSubjectAreaDidChange, object: nil, queue: nil) { [weak self] n in
            self?.filaSessao.async { self?.cenaMudou(n.object as? AVCaptureDevice) }
        })
        observadores.append(c.addObserver(forName: .AVCaptureSessionRuntimeError, object: sessao, queue: nil) { [weak self] n in
            let e = n.userInfo?[AVCaptureSessionErrorKey] as? NSError
            let msg = e.map { "\($0.domain) \($0.code) \($0.localizedDescription)" } ?? "desconhecido"
            Log.captura.notice("fmcam captura: erro de execução da câmera: \(msg, privacy: .public)")
            self?.filaSessao.async { self?.tratarErroDeExecucao(msg) }
        })
    }

    deinit { observadores.forEach(NotificationCenter.default.removeObserver) }

    // MARK: ciclo de vida (primeiro plano)

    /// A câmera abre no formato salvo no iPhone (senão a 1a conexão em 4K reconfigurava a sessão dentro do limite de partida).
    func definirAjustesIniciais(_ a: AjustesCamera) { filaSessao.async { self.ajustes = a; self.preset = a.preset } }

    func ligarEmPrimeiroPlano() {
        filaSessao.async {
            let repetido = !ControleDePartida.deveLigarPreview(jaEmPrimeiroPlano: self.emPrimeiroPlano, sessaoRodando: self.sessao.isRunning,
                                                               configurada: self.dimAtual != nil)
            self.emPrimeiroPlano = true
            if repetido {
                Log.captura.notice("fmcam captura: primeiro plano repetido, a câmera já está ligada")
                return
            }
            Log.captura.notice("fmcam captura: primeiro plano, ligando a câmera")
            self.garantir(preset: self.preset, completion: nil)
        }
    }

    func desligar() {
        filaSessao.async {
            self.emPrimeiroPlano = false
            Log.captura.notice("fmcam captura: segundo plano, parando a câmera")
            if self.sessao.isRunning { self.sessao.stopRunning() }
            self.aoEstado?(.desligada)
        }
    }

    // MARK: MotorDeCaptura

    func prepararParaTransmitir(_ config: ConfigVideo, cancelada: @escaping () -> Bool,
                                completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        let pedidoEm = Date()
        filaSessao.async {
            // Diagnóstico da partida: o relógio do limite de 2,5 s corre desde o handshake, inclusive enquanto esta fila
            // estava presa em outra coisa (startRunning do preview, por exemplo).
            let espera = Int(Date().timeIntervalSince(pedidoEm) * 1000)
            Log.captura.notice("fmcam captura: partida pedida esperou \(espera, privacy: .public) ms na fila da câmera, sessão rodando=\(self.sessao.isRunning, privacy: .public), formato atual \(self.preset.rawValue, privacy: .public), pedido \(config.preset.rawValue, privacy: .public)")
            self.preset = config.preset
            // Na conexão o pedido de lente do OBS vale.
            let pedido: AjustesCamera.Mutacao = { $0 = $0.aplicando(config: config) }
            let novo = self.ajustes.aplicando(config: config)
            if novo != self.ajustes {
                self.ajustes = novo
                self.aoMutacao?(pedido)
            }
            self.garantir(preset: config.preset, cancelada: cancelada, completion: completion)
        }
    }

    func definirConsumidor(dono: AnyObject, amostra: @escaping (CMSampleBuffer) -> Void,
                           aoFalha: @escaping (String) -> Void, aoReconfigurar: @escaping (DimensoesVideo) -> Void) {
        trava.lock()
        consumidor = Consumidor(dono: dono, amostra: amostra, aoFalha: aoFalha, aoReconfigurar: aoReconfigurar)
        trava.unlock()
    }

    func mudarPreset(_ novo: Preset, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        filaSessao.async {
            Log.captura.notice("fmcam captura: trocando o formato de \(self.preset.rawValue, privacy: .public) para \(novo.rawValue, privacy: .public)")
            self.preset = novo
            self.garantir(preset: novo, completion: completion)
        }
    }

    /// Qualidade trocada no iPhone sem conexão: o preview passa para o formato salvo. Com cliente (a sessão cuida da troca)
    /// ou em segundo plano (câmera parada), nada muda aqui.
    func mudarPresetSemConexao(_ novo: Preset) {
        filaSessao.async {
            guard self.emPrimeiroPlano, self.consumidorAtual() == nil, novo != self.preset || self.dimAtual == nil else { return }
            Log.captura.notice("fmcam captura: qualidade salva mudou sem conexão, preview de \(self.preset.rawValue, privacy: .public) para \(novo.rawValue, privacy: .public)")
            self.preset = novo
            self.garantir(preset: novo, completion: nil)
        }
    }

    func liberarConsumidor(dono: AnyObject) {
        trava.lock()
        if let c = consumidor, c.dono === dono || c.dono == nil { consumidor = nil }
        trava.unlock()
    }

    private func consumidorAtual() -> Consumidor? {
        trava.lock(); defer { trava.unlock() }
        return consumidor
    }

    // MARK: ajustes ao vivo

    /// Mudança feita no iPhone. Vale na hora (o último que mudou ganha).
    /// `rampa`: veio do botão de lente (no dispositivo virtual vira rampa de zoom em vez de troca de entrada).
    func definirAjustes(_ novo: AjustesCamera, rampa: Bool = false) {
        filaSessao.async {
            let antigo = self.ajustes
            self.ajustes = novo
            guard self.dimAtual != nil else { return }
            if rampa, self.mapaVirtual != nil, novo.lente != antigo.lente || novo.zoom != antigo.zoom {
                self.rampar(novo)
                // o resto dos ajustes vale na hora, sem mexer no zoom (a rampa cuida dele)
                var ant = antigo
                ant.lente = novo.lente
                ant.zoom = novo.zoom
                self.aplicarAjustes(antes: ant)
                return
            }
            if ControleManual.precisa(novo) != ControleManual.precisa(antigo), Captura.dispositivoVirtual() != nil {
                // Foco manual ou branco em K ligou ou desligou: o virtual não tem esses recursos, então troca de câmera
                // (física para o manual, virtual de volta quando sai). Mesmo caminho da troca de lente: tipo 1 e chave.
                Log.captura.notice("fmcam captura: controle manual mudou (foco manual=\(novo.focoModo == .manual, privacy: .public), branco em K=\(novo.brancoTravado && novo.kelvin != nil, privacy: .public)), reavaliando a câmera")
                self.garantir(preset: self.preset) { r in
                    if case .success(let dim) = r { self.consumidorAtual()?.aoReconfigurar(dim) }
                }
            } else if novo.lente != antigo.lente, self.mapaVirtual == nil {
                Log.captura.notice("fmcam captura: trocando de lente \(antigo.lente.rawValue, privacy: .public) para \(novo.lente.rawValue, privacy: .public)")
                self.garantir(preset: self.preset) { r in
                    if case .success(let dim) = r { self.consumidorAtual()?.aoReconfigurar(dim) }
                }
            } else {
                self.aplicarAjustes(antes: antigo)
                if novo.estabilizar != antigo.estabilizar {
                    self.aplicarEstabilizacao()
                    self.aplicarAjustes()  // mexer na estabilização não pode deixar o foco em outro modo
                }
            }
        }
    }

    /// Toque para focar. `travar` (toque longo) trava foco e exposição no ponto depois de assentar.
    func focar(ponto: CGPoint, travar: Bool) {
        filaSessao.async {
            guard let d = self.dispositivoAtual else { return }
            let ponto = LimitesDeCamera.ponto(ponto)
            // Em foco ou exposição manual o toque não tem o que fazer: o dono já escolheu o valor.
            let a = self.ajustes
            do { try d.lockForConfiguration() } catch { return }
            if a.focoModo != .manual {
                // Fica contínuo naquele ponto até a cena mudar (aí volta ao centro). Nunca foco único.
                if d.isFocusPointOfInterestSupported { d.focusPointOfInterest = ponto }
                if d.isFocusModeSupported(.continuousAutoFocus) { d.focusMode = .continuousAutoFocus }
                d.isSubjectAreaChangeMonitoringEnabled = true
            }
            if a.exposicaoModo != .manual {
                if d.isExposurePointOfInterestSupported { d.exposurePointOfInterest = ponto }
                if d.isExposureModeSupported(.continuousAutoExposure) { d.exposureMode = .continuousAutoExposure }
            }
            d.unlockForConfiguration()
            self.pontoTocado = ponto
            let destravar: AjustesCamera.Mutacao = { $0.focoTravado = false; $0.exposicaoTravada = false }
            destravar(&self.ajustes)
            self.aoMutacao?(destravar)
            guard travar else { return }
            self.filaSessao.asyncAfter(deadline: .now() + 0.8) {
                let antes = self.ajustes
                let trancar: AjustesCamera.Mutacao = { $0.focoTravado = true; $0.exposicaoTravada = true }
                trancar(&self.ajustes)
                self.aplicarAjustes(antes: antes)
                self.aoMutacao?(trancar)
            }
        }
    }

    // MARK: núcleo (fila da câmera)

    private func garantir(preset: Preset, cancelada: (() -> Bool)? = nil,
                          completion: ((Result<DimensoesVideo, ErroFonte>) -> Void)?) {
        if let c = cancelada, !ControleDePartida.deveProsseguir(emPrimeiroPlano: emPrimeiroPlano, cancelada: c()) {
            Log.captura.notice("fmcam captura: partida cancelada antes da autorização, câmera não liga")
            completion?(.failure(.camera("Partida cancelada.")))
            return
        }
        guard let dispositivo = escolherDispositivo(preset: preset) else {
            aoEstado?(.semCamera)
            completion?(.failure(.camera("Este aparelho não tem câmera traseira.")))
            return
        }
        Captura.autorizar { ok in
            self.filaSessao.async { self.continuar(dispositivo, autorizado: ok, preset: preset, cancelada: cancelada, completion: completion) }
        }
    }

    /// Dispositivo virtual quando ele tem o formato (e a estabilização, se ligada). Senão a câmera física da lente,
    /// que é o comportamento de antes, e o motivo vai para o log.
    private func escolherDispositivo(preset: Preset) -> AVCaptureDevice? {
        let virtual = Captura.dispositivoVirtual()
        let formato = virtual.flatMap { Captura.formato($0, preset) }
        var estabiliza = false
        if let f = formato {
            estabiliza = f.isVideoStabilizationModeSupported(.standard)
            if #available(iOS 17.0, *), f.isVideoStabilizationModeSupported(.previewOptimized) { estabiliza = true }
        }
        let serve = virtual.map {
            ControleManual.dispositivoServe(focoManual: ajustes.focoModo == .manual, brancoEmK: ajustes.brancoTravado && ajustes.kelvin != nil,
                                            suportaLenteCustom: $0.isLockingFocusWithCustomLensPositionSupported,
                                            suportaGanhosCustom: $0.isLockingWhiteBalanceWithCustomDeviceGainsSupported)
        } ?? true
        let modo = ModoCamera.escolher(virtualExiste: virtual != nil, virtualTemFormato: formato != nil,
                                       estabilizar: ajustes.estabilizar, virtualEstabiliza: estabiliza, virtualServeAoManual: serve)
        if case .virtual = modo, let v = virtual { return v }
        if case .fisica(let motivo) = modo, virtual != nil {
            Log.captura.notice("fmcam captura: dispositivo virtual não serve para \(preset.rawValue, privacy: .public) (\(motivo, privacy: .public)), usando a câmera física da lente")
        }
        return Captura.dispositivo(para: ajustes.lente)
    }

    private func continuar(_ dispositivo: AVCaptureDevice, autorizado: Bool, preset: Preset, cancelada: (() -> Bool)?,
                           completion: ((Result<DimensoesVideo, ErroFonte>) -> Void)?) {
        // O cliente pode ter saído durante o diálogo de permissão: não liga câmera por quem já foi embora.
        if let c = cancelada, !ControleDePartida.deveProsseguir(emPrimeiroPlano: emPrimeiroPlano, cancelada: c()) {
            Log.captura.notice("fmcam captura: partida cancelada durante a autorização, câmera não liga")
            completion?(.failure(.camera("Partida cancelada.")))
            return
        }
        guard autorizado else {
            aoEstado?(.semPermissao)
            completion?(.failure(.camera("Sem permissão para a câmera. Libere em Ajustes.")))
            return
        }
        do {
            let ehVirtual = Captura.lente(de: dispositivo) == nil && dispositivo.constituentDevices.count > 1
            let mapa = ehVirtual ? Captura.mapaZoom(dispositivo) : nil
            // No virtual a "lente" é só um ponto de zoom: vale a pedida, se o aparelho a tem.
            let lenteReal = ehVirtual ? (mapa?.fatorBase(ajustes.lente) != nil ? ajustes.lente : .wide) : (Captura.lente(de: dispositivo) ?? .wide)
            if lenteReal != ajustes.lente {
                Log.captura.notice("fmcam captura: lente \(self.ajustes.lente.rawValue, privacy: .public) indisponível, usando \(lenteReal.rawValue, privacy: .public)")
                let queda: AjustesCamera.Mutacao = { $0.lente = lenteReal; $0.zoom = 1 }
                queda(&ajustes)
                aoMutacao?(queda)
            }
            let dim: DimensoesVideo
            let chaveLente: Lente = ehVirtual ? .wide : lenteReal
            if let atual = dimAtual, let c = configurado, c.preset == preset, c.lente == chaveLente, dispositivoAtual === dispositivo {
                dim = atual
            } else {
                aoEstado?(.ligando)
                dim = try configurarSessao(preset: preset, dispositivo: dispositivo, lente: chaveLente)
            }
            mapaVirtual = mapa
            if mapa != nil { prepararTrocaDeCamera(dispositivo) } else { obsConstituinte = nil; dispositivoObservado = nil }
            aplicarAjustes()
            publicarLimites(dispositivo, dim)
            if !sessao.isRunning {
                reinicio.zerar()
                let t0 = Date()
                sessao.startRunning()
                let ms = Int(Date().timeIntervalSince(t0) * 1000)
                Log.captura.notice("fmcam captura: startRunning levou \(ms, privacy: .public) ms, isRunning=\(self.sessao.isRunning, privacy: .public)")
            }
            aoEstado?(.ligada)
            completion?(.success(dim))
        } catch let e as ErroFonte {
            aoEstado?(.erro(e.mensagem))
            completion?(.failure(e))
        } catch {
            aoEstado?(.erro("Falha ao iniciar a câmera."))
            completion?(.failure(.camera("Falha ao iniciar a câmera.")))
        }
    }

    /// Escolhe formato e fps (cai para 30 e depois 720p se o aparelho não tem), troca entrada e saída.
    private func configurarSessao(preset: Preset, dispositivo: AVCaptureDevice, lente: Lente) throws -> DimensoesVideo {
        let candidatos: [(Int, Int, Int)] = preset.quedas.map { ($0.largura, $0.altura, $0.fps) }
        var escolhido: (AVCaptureDevice.Format, DimensoesVideo)?
        for (w, h, fps) in candidatos {
            let formatos = dispositivo.formats.filter { f in
                let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
                return Int(d.width) == w && Int(d.height) == h
                    && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
            }
            if let f = formatos.first(where: { !$0.isVideoBinned }) ?? formatos.first {
                escolhido = (f, DimensoesVideo(largura: w, altura: h, fps: fps))
                break
            }
        }
        guard let (formato, dim) = escolhido else {
            throw ErroFonte.camera("A câmera não oferece o formato pedido.")
        }
        let saida: AVCaptureVideoDataOutput
        let entradaAtual = sessao.inputs.compactMap { $0 as? AVCaptureDeviceInput }.first
        if ReusoDaSessao.mantemEntrada(mesmoDispositivo: entradaAtual?.device === dispositivo, entradas: sessao.inputs.count,
                                       saidaAtualNaSessao: saidaAtual.map { sessao.outputs.contains($0) } ?? false),
           let s = saidaAtual {
            // Mesma câmera, só outro formato (troca de qualidade): tirar e pôr a entrada de volta reiniciava as câmeras do
            // dispositivo virtual e o vídeo parava 1 a 1,6 s (medido no 15 Pro Max, encostando no watchdog de 1,5 s).
            saida = s
        } else {
            let entrada: AVCaptureDeviceInput
            do { entrada = try AVCaptureDeviceInput(device: dispositivo) }
            catch { throw ErroFonte.camera("Não foi possível abrir a câmera.") }

            saida = AVCaptureVideoDataOutput()
            saida.alwaysDiscardsLateVideoFrames = true
            saida.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            saida.setSampleBufferDelegate(self, queue: filaAmostras)

            sessao.beginConfiguration()
            sessao.inputs.forEach { sessao.removeInput($0) }
            sessao.outputs.forEach { sessao.removeOutput($0) }
            guard sessao.canAddInput(entrada), sessao.canAddOutput(saida) else {
                sessao.commitConfiguration()
                throw ErroFonte.camera("A câmera não pôde ser ligada.")
            }
            sessao.addInput(entrada)
            sessao.addOutput(saida)
            sessao.commitConfiguration()
        }

        do {
            try dispositivo.lockForConfiguration()
            dispositivo.activeFormat = formato
            let duracao = CMTime(value: 1, timescale: CMTimeScale(dim.fps))
            dispositivo.activeVideoMinFrameDuration = duracao
            dispositivo.activeVideoMaxFrameDuration = duracao
            dispositivo.unlockForConfiguration()
        } catch {
            throw ErroFonte.camera("Não foi possível ajustar a câmera.")
        }
        // O vídeo enviado segue a posição física do aparelho: paisagem (0 ou 180) ou em pé (90 ou 270).
        let angulo = monitorOrientacao.anguloAplicado
        var anguloUsado = 0
        if let conn = saida.connection(with: .video), Self.definirAngulo(conn, angulo) { anguloUsado = angulo }
        anguloConexao = anguloUsado
        monitorOrientacao.usar(dispositivo: dispositivo)
        let dimEnviada = OrientacaoVideo.dimensoes(dim, angulo: anguloUsado)
        saidaAtual = saida
        dispositivoAtual = dispositivo
        configurado = (preset, lente)
        dimAtual = dimEnviada
        pontoTocado = nil
        trava.lock(); logPrimeiroQuadro = true; trava.unlock()
        aplicarEstabilizacao()
        Log.captura.notice("fmcam captura: configurada, lente \(lente.rawValue, privacy: .public), \(dimEnviada.largura, privacy: .public)x\(dimEnviada.altura, privacy: .public) a \(dimEnviada.fps, privacy: .public) fps, rotação \(anguloUsado, privacy: .public) graus")
        return dimEnviada
    }

    /// Põe o ângulo na conexão (RotationCoordinator no iOS 17, videoOrientation antes). false se a conexão não aceita.
    private static func definirAngulo(_ conn: AVCaptureConnection, _ a: Int) -> Bool {
        if #available(iOS 17.0, *) {
            guard conn.isVideoRotationAngleSupported(CGFloat(a)) else { return false }
            conn.videoRotationAngle = CGFloat(a)
            return true
        }
        guard conn.isVideoOrientationSupported else { return false }
        conn.videoOrientation = OrientacaoVideo.orientacaoDeVideo(a)
        return true
    }

    /// O aparelho girou (já passou pelo debounce): gira a saída e avisa o codificador do tamanho novo, que manda tipo 1 e
    /// quadro-chave. Sem sessão configurada não há o que girar: a próxima configuração já nasce no ângulo certo.
    private func aplicarRotacao(_ a: Int) {
        guard a != anguloConexao, let conn = saidaAtual?.connection(with: .video), let atual = dimAtual else { return }
        guard Self.definirAngulo(conn, a) else {
            Log.captura.notice("fmcam captura: a conexão não aceita a rotação de \(a, privacy: .public) graus")
            return
        }
        let nova = OrientacaoVideo.reorientar(atual, de: anguloConexao, para: a)
        Log.captura.notice("fmcam captura: rotação de \(self.anguloConexao, privacy: .public) para \(a, privacy: .public) graus, \(nova.largura, privacy: .public)x\(nova.altura, privacy: .public)")
        anguloConexao = a
        dimAtual = nova
        aoDimensao?(nova)
        consumidorAtual()?.aoReconfigurar(nova)
    }

    /// Padrão desligado: a estabilização segura quadros e soma atraso. Ligada, usa o modo de menor atraso que o formato
    /// aceita (nunca `.auto` nem cinematic, que seguram muitos quadros). Não mexe em lente nem em zoom.
    private func aplicarEstabilizacao() {
        guard let conn = saidaAtual?.connection(with: .video) else { return }
        defer {
            Log.captura.notice("fmcam captura: estabilização pedida=\(self.ajustes.estabilizar, privacy: .public), modo ativo=\(conn.activeVideoStabilizationMode.rawValue, privacy: .public) (0 desligada, 1 standard, 2 cinematic, 3 extended, 4 previewOptimized, -1 auto)")
        }
        guard ajustes.estabilizar else {
            conn.preferredVideoStabilizationMode = .off
            return
        }
        var suportados = Set<ModoEstabilizacao>()
        if conn.isVideoStabilizationSupported, let f = dispositivoAtual?.activeFormat {
            if f.isVideoStabilizationModeSupported(.standard) { suportados.insert(.standard) }
            if #available(iOS 17.0, *), f.isVideoStabilizationModeSupported(.previewOptimized) { suportados.insert(.previewOptimized) }
        }
        guard let modo = Estabilizacao.escolher(suportados: suportados) else {
            conn.preferredVideoStabilizationMode = .off
            var a = ajustes; a.estabilizar = false; ajustes = a
            aoMutacao?({ $0.estabilizar = false })
            aoAviso?("A estabilização não está disponível nesta lente e formato.")
            return
        }
        if modo == .previewOptimized, #available(iOS 17.0, *) {
            conn.preferredVideoStabilizationMode = .previewOptimized
        } else {
            conn.preferredVideoStabilizationMode = .standard
        }
    }

    /// Aplica só o que mudou desde `antes` (nil = tudo): assim mexer na exposição não desfaz o toque de foco.
    private func aplicarAjustes(antes: AjustesCamera? = nil) {
        guard let d = dispositivoAtual else { return }
        do { try d.lockForConfiguration() } catch {
            Log.captura.notice("fmcam captura: não consegui travar a câmera para ajustar")
            return
        }
        // Se o app morrer aqui dentro, a marca sobra e a próxima abertura volta sem travas (ver ProtecaoDeCrash).
        let protecao = ProtecaoDeCrash()
        protecao.marcar()
        defer {
            d.unlockForConfiguration()
            protecao.limpar()
            // o que o dispositivo ficou fazendo de verdade (os setters são assíncronos): vai para o log do aparelho
            filaSessao.asyncAfter(deadline: .now() + 0.7) { [weak self] in self?.registrarEstadoEfetivo() }
        }
        let a = ajustes
        func mudou(_ f: (AjustesCamera) -> [AnyHashable]) -> Bool {
            guard let antes else { return true }
            return f(antes) != f(a)
        }

        if mudou({ [$0.zoom, $0.lente] }) {
            if let alvo = fatorVirtual(a, d) {
                // Pinça ou partida: vai direto (a rampa é só para o toque nos botões de lente).
                d.cancelVideoZoomRamp()
                d.videoZoomFactor = CGFloat(zoomSeguro(alvo, d))
            } else if mapaVirtual == nil {
                let maxZoom = min(Double(d.activeFormat.videoMaxZoomFactor), AjustesCamera.faixaZoom.upperBound)
                d.videoZoomFactor = CGFloat(zoomSeguro(min(max(a.zoom, 1), maxZoom), d))
            }
        }

        if mudou({ [$0.exposicaoModo, $0.exposicaoTravada, $0.bias, $0.iso, $0.obturador] }) {
            if a.exposicaoModo == .manual, d.isExposureModeSupported(.custom) {
                let f = d.activeFormat
                let iso = LimitesDeCamera.limitarF(Float(a.iso), f.minISO, f.maxISO, padrao: d.iso)
                let seg = LimitesDeCamera.obturador(a.obturador, min: f.minExposureDuration.seconds, max: f.maxExposureDuration.seconds,
                                                    duracaoQuadro: d.activeVideoMinFrameDuration.seconds, padrao: d.exposureDuration.seconds)
                let dur = CMTime(seconds: seg, preferredTimescale: 1_000_000_000)
                d.setExposureModeCustom(duration: dur, iso: iso, completionHandler: nil)
            } else if a.exposicaoModo != .manual {
                let bias = LimitesDeCamera.limitarF(Float(a.bias), d.minExposureTargetBias, d.maxExposureTargetBias, padrao: 0)
                d.setExposureTargetBias(bias, completionHandler: nil)
                if a.exposicaoTravada {
                    if d.exposureMode != .locked, d.isExposureModeSupported(.locked) { d.exposureMode = .locked }
                } else if d.exposureMode != .continuousAutoExposure, d.isExposureModeSupported(.continuousAutoExposure) {
                    d.exposureMode = .continuousAutoExposure
                }
            } else {
                Log.captura.notice("fmcam captura: exposição manual indisponível neste dispositivo, ignorada")
            }
        }

        if mudou({ [$0.focoModo, $0.focoTravado, $0.posicaoLente] }) {
            if a.focoModo == .manual {
                if d.isLockingFocusWithCustomLensPositionSupported {
                    d.setFocusModeLocked(lensPosition: LimitesDeCamera.limitarF(Float(a.posicaoLente), 0, 1, padrao: 0.5), completionHandler: nil)
                }
            } else if a.focoTravado {
                if d.focusMode != .locked, d.isFocusModeSupported(.locked) { d.focusMode = .locked }
            } else {
                if d.isFocusModeSupported(.continuousAutoFocus) { d.focusMode = .continuousAutoFocus }
                if d.isFocusPointOfInterestSupported { d.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5) }
            }
            d.isSubjectAreaChangeMonitoringEnabled = PlanoDeFoco.de(a).monitorarCena
            Log.captura.notice("fmcam captura: foco pedido=\(a.focoModo.rawValue, privacy: .public) travado=\(a.focoTravado, privacy: .public), modo efetivo do dispositivo=\(d.focusMode.rawValue, privacy: .public) (0 travado, 1 automático único, 2 contínuo)")
        }

        if mudou({ [$0.brancoTravado, $0.kelvin ?? -1] }) {
            if a.brancoTravado {
                if d.whiteBalanceMode != .locked || a.kelvin != nil { travarBranco(d, kelvin: a.kelvin) }
            } else if d.whiteBalanceMode == .locked, d.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                d.whiteBalanceMode = .continuousAutoWhiteBalance
            }
        }
    }

    /// Modos efetivos do dispositivo, para conferir no log se o que foi pedido pegou.
    func registrarEstadoEfetivo() {
        guard let d = dispositivoAtual else { return }
        let conn = saidaAtual?.connection(with: .video)
        let f = CMVideoFormatDescriptionGetDimensions(d.activeFormat.formatDescription)
        Log.captura.notice("fmcam estado: balanço=\(d.whiteBalanceMode.rawValue, privacy: .public) exposição=\(d.exposureMode.rawValue, privacy: .public) foco=\(d.focusMode.rawValue, privacy: .public) lente=\(d.lensPosition, format: .fixed(precision: 2), privacy: .public) bias=\(d.exposureTargetBias, format: .fixed(precision: 2), privacy: .public) iso=\(Int(d.iso), privacy: .public) obt=\(d.exposureDuration.seconds, format: .fixed(precision: 5), privacy: .public) zoom=\(d.videoZoomFactor, format: .fixed(precision: 2), privacy: .public) estab(ativa=\(conn?.activeVideoStabilizationMode.rawValue ?? -9, privacy: .public) pref=\(conn?.preferredVideoStabilizationMode.rawValue ?? -9, privacy: .public)) formato=\(f.width, privacy: .public)x\(f.height, privacy: .public) ajustes(wb=\(self.ajustes.brancoTravado, privacy: .public) ae=\(self.ajustes.exposicaoTravada, privacy: .public) af=\(self.ajustes.focoTravado, privacy: .public) mf=\(self.ajustes.focoModo.rawValue, privacy: .public) estab=\(self.ajustes.estabilizar, privacy: .public)) gravandoBranco=\(d.isLockingWhiteBalanceWithCustomDeviceGainsSupported, privacy: .public) maxGanho=\(d.maxWhiteBalanceGain, format: .fixed(precision: 2), privacy: .public)")
    }

    /// Trava o branco só pelo que o dispositivo de agora suporta, com ganhos limitados a 1...maxWhiteBalanceGain dele.
    /// Os ganhos custom exigem `isLockingWhiteBalanceWithCustomDeviceGainsSupported` (o dispositivo virtual pode não ter) e
    /// um máximo válido (zero antes da sessão rodar). Sem eles, `whiteBalanceMode = .locked`, e sem isso só log.
    private func travarBranco(_ d: AVCaptureDevice, kelvin: Double?) {
        var ganhos: LimitesDeCamera.Ganhos?
        if d.isLockingWhiteBalanceWithCustomDeviceGainsSupported {
            var g = d.deviceWhiteBalanceGains
            if let k = kelvin {
                g = d.deviceWhiteBalanceGains(for: .init(temperature: LimitesDeCamera.kelvinSeguro(k), tint: 0))
            }
            ganhos = LimitesDeCamera.ganhosSeguros(r: g.redGain, g: g.greenGain, b: g.blueGain, max: d.maxWhiteBalanceGain)
        }
        switch PlanoBranco.decidir(suportaGanhos: d.isLockingWhiteBalanceWithCustomDeviceGainsSupported,
                                   suportaTravado: d.isWhiteBalanceModeSupported(.locked), ganhos: ganhos) {
        case .ganhos(let g):
            d.setWhiteBalanceModeLocked(with: .init(redGain: g.r, greenGain: g.g, blueGain: g.b), completionHandler: nil)
        case .modoTravado:
            if d.whiteBalanceMode != .locked { d.whiteBalanceMode = .locked }
        case .ignorar(let motivo):
            Log.captura.notice("fmcam captura: trava de branco ignorada, \(motivo, privacy: .public)")
        }
    }

    private func zoomSeguro(_ z: Double, _ d: AVCaptureDevice) -> Double {
        LimitesDeCamera.zoom(z, min: Double(d.minAvailableVideoZoomFactor), max: Double(d.maxAvailableVideoZoomFactor))
    }

    /// Troca de câmera física do virtual: comportamento .auto sem condições restritivas e todas as câmeras mais curtas permitidas
    /// como reserva, para o sistema cair para a lente que foca (distância mínima de foco, pouca luz) como a câmera nativa.
    /// Nunca força câmera física. Também liga o log de qual câmera está ativa.
    private func prepararTrocaDeCamera(_ d: AVCaptureDevice) {
        if d.primaryConstituentDeviceSwitchingBehavior != .unsupported {
            do {
                try d.lockForConfiguration()
                d.setPrimaryConstituentDeviceSwitchingBehavior(.auto, restrictedSwitchingBehaviorConditions: [])
                d.fallbackPrimaryConstituentDevices = d.supportedFallbackPrimaryConstituentDevices
                d.unlockForConfiguration()
            } catch {
                Log.captura.notice("fmcam captura: não consegui definir o comportamento de troca de câmera")
            }
            Log.captura.notice("fmcam captura: troca de câmera automática, reservas \(d.fallbackPrimaryConstituentDevices.count, privacy: .public) de \(d.supportedFallbackPrimaryConstituentDevices.count, privacy: .public), comportamento \(d.primaryConstituentDeviceSwitchingBehavior.rawValue, privacy: .public) (1 auto, 2 restrito, 3 travado)")
        }
        guard dispositivoObservado !== d else { return }
        dispositivoObservado = d
        obsConstituinte = d.observe(\.activePrimaryConstituent, options: [.initial, .new]) { dev, _ in
            Log.captura.notice("fmcam captura: câmera física ativa \(Self.nomeDaCamera(dev.activePrimaryConstituent), privacy: .public), zoom \(dev.videoZoomFactor, format: .fixed(precision: 2), privacy: .public), comportamento de troca \(dev.primaryConstituentDeviceSwitchingBehavior.rawValue, privacy: .public), condições \(dev.primaryConstituentDeviceRestrictedSwitchingBehaviorConditions.rawValue, privacy: .public)")
        }
    }

    private static func nomeDaCamera(_ d: AVCaptureDevice?) -> String {
        guard let d else { return "nenhuma" }
        return Captura.lente(de: d)?.rawValue ?? d.deviceType.rawValue
    }

    /// Fator de `videoZoomFactor` no dispositivo virtual para a lente e o zoom pedidos (dentro dos limites do formato).
    private func fatorVirtual(_ a: AjustesCamera, _ d: AVCaptureDevice) -> Double? {
        guard let mapa = mapaVirtual, let f = mapa.fator(lente: a.lente, zoom: a.zoom) else { return nil }
        let teto = min(Double(d.maxAvailableVideoZoomFactor), (mapa.fatorBase(a.lente) ?? 1) * AjustesCamera.faixaZoom.upperBound)
        return LimitesDeCamera.zoom(min(max(f, Double(d.minAvailableVideoZoomFactor)), teto), min: Double(d.minAvailableVideoZoomFactor), max: Double(d.maxAvailableVideoZoomFactor))
    }

    /// Botão de lente no dispositivo virtual: rampa curta de zoom. Não reconfigura a sessão, não muda o codificador,
    /// não força quadro-chave nem câmera física: o sistema escolhe a que foca.
    private func rampar(_ a: AjustesCamera) {
        guard let d = dispositivoAtual, let alvo = fatorVirtual(a, d) else { return }
        do { try d.lockForConfiguration() } catch { return }
        defer { d.unlockForConfiguration() }
        guard let taxa = RampaZoom.taxa(de: Double(d.videoZoomFactor), para: alvo), taxa.isFinite, taxa > 0 else {
            d.videoZoomFactor = CGFloat(zoomSeguro(alvo, d)); return
        }
        Log.captura.notice("fmcam captura: rampa de zoom \(d.videoZoomFactor, format: .fixed(precision: 2), privacy: .public) para \(alvo, format: .fixed(precision: 2), privacy: .public) (lente \(a.lente.rawValue, privacy: .public))")
        d.ramp(toVideoZoomFactor: CGFloat(alvo), withRate: taxa)
        filaSessao.asyncAfter(deadline: .now() + RampaZoom.duracaoPadrao + 0.2) {
            guard let d = self.dispositivoAtual else { return }
            Log.captura.notice("fmcam captura: rampa terminou em \(d.videoZoomFactor, format: .fixed(precision: 2), privacy: .public), câmera física ativa \(Self.nomeDaCamera(d.activePrimaryConstituent), privacy: .public) (lente pedida \(a.lente.rawValue, privacy: .public))")
        }
    }

    /// A cena mudou: se o usuário tinha tocado num ponto fora do centro, volta ao centro uma vez. Sem ponto tocado não faz nada
    /// (não reescreve modo nem ponto, para não reiniciar a busca do foco). Só reescreve o que de fato está diferente.
    private func cenaMudou(_ d: AVCaptureDevice?) {
        guard let atual = dispositivoAtual, d === atual, PlanoDeFoco.devolveAoCentro(ajustes) else { return }
        let agora = Date()
        guard CenaMudou.devolver(pontoTocado: pontoTocado, ultimaVez: ultimaDevolucao, agora: agora) else { return }
        do { try atual.lockForConfiguration() } catch { return }
        let centro = CenaMudou.centro
        if atual.isFocusPointOfInterestSupported { atual.focusPointOfInterest = centro }
        if atual.isFocusModeSupported(.continuousAutoFocus), atual.focusMode != .continuousAutoFocus { atual.focusMode = .continuousAutoFocus }
        if ajustes.exposicaoModo == .auto, !ajustes.exposicaoTravada {
            if atual.isExposurePointOfInterestSupported { atual.exposurePointOfInterest = centro }
            if atual.isExposureModeSupported(.continuousAutoExposure), atual.exposureMode != .continuousAutoExposure { atual.exposureMode = .continuousAutoExposure }
        }
        atual.unlockForConfiguration()
        pontoTocado = nil
        ultimaDevolucao = agora
        Log.captura.notice("fmcam captura: cena mudou, foco e exposição de volta ao centro (foco efetivo=\(atual.focusMode.rawValue, privacy: .public))")
    }

    /// ISO, obturador e posição da lente de agora.
    private func lerCamera() {
        guard let d = dispositivoAtual, sessao.isRunning else { return }
        aoLeitura?(LeituraCamera(iso: Int(d.iso.rounded()), obturadorSegundos: d.exposureDuration.seconds,
                                 posicaoLente: Double(d.lensPosition)))
    }

    /// O setter do AVFoundation lança exceção se algum ganho sair de 1...max.
    private static func normalizar(_ g: AVCaptureDevice.WhiteBalanceGains, max m: Float) -> AVCaptureDevice.WhiteBalanceGains {
        func c(_ v: Float) -> Float { min(max(v, 1), m) }
        return AVCaptureDevice.WhiteBalanceGains(redGain: c(g.redGain), greenGain: c(g.greenGain), blueGain: c(g.blueGain))
    }

    private func publicarLimites(_ d: AVCaptureDevice, _ dim: DimensoesVideo) {
        let f = d.activeFormat
        let maxObt = min(f.maxExposureDuration.seconds, d.activeVideoMinFrameDuration.seconds)
        let ev = LimitesCamera.faixaEV(aparelhoMin: Double(d.minExposureTargetBias), aparelhoMax: Double(d.maxExposureTargetBias))
        aoLimites?(LimitesCamera(isoMin: Double(f.minISO), isoMax: Double(f.maxISO),
                                 obturadorMin: f.minExposureDuration.seconds, obturadorMax: maxObt,
                                 evMin: ev.min, evMax: ev.max))
        aoDimensao?(dim)
    }

    private func tratarErroDeExecucao(_ msg: String) {
        guard emPrimeiroPlano || consumidorAtual() != nil else { return }
        guard reinicio.podeTentar(agora: Date()) else {
            aoEstado?(.erro("A câmera parou: \(msg)"))
            consumidorAtual()?.aoFalha("A câmera parou: \(msg)")
            return
        }
        Log.captura.notice("fmcam captura: tentando religar a câmera uma vez")
        sessao.startRunning()
        Log.captura.notice("fmcam captura: religar, isRunning=\(self.sessao.isRunning, privacy: .public)")
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        trava.lock()
        let primeiro = logPrimeiroQuadro
        logPrimeiroQuadro = false
        let c = consumidor
        trava.unlock()
        if primeiro, let pb = CMSampleBufferGetImageBuffer(sampleBuffer) {
            let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
            Log.captura.notice("fmcam captura: primeiro quadro \(w, privacy: .public)x\(h, privacy: .public)\(h > w ? " (em pé)" : "", privacy: .public)")
        }
        c?.amostra(sampleBuffer)
    }
}

// MARK: - Fonte real: motor de captura + codificador

final class FonteCamera: FonteDeVideo {
    var aoEvento: ((EventoVideo) -> Void)?

    private let motor: MotorDeCaptura
    private let criarCodificador: (Int, Int, Int, Int) throws -> Codificador
    private var codificador: Codificador?
    private var bitrate = PedidoHandshake.bitratePadrao
    private var preset = Preset.p1080_30
    private var parado = false
    private let trava = NSLock()

    init(motor: MotorDeCaptura,
         criarCodificador: @escaping (Int, Int, Int, Int) throws -> Codificador = {
             try Codificador(largura: $0, altura: $1, fps: $2, bitrate: $3)
         }) {
        self.motor = motor
        self.criarCodificador = criarCodificador
    }

    func iniciar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        bitrate = config.bitrate
        preset = config.preset
        motor.prepararParaTransmitir(config, cancelada: { [weak self] in self?.estaParado ?? true }) { [weak self] r in
            guard let self else { return }
            switch r {
            case .failure(let e):
                completion(.failure(e))
            case .success(let dim):
                self.trava.lock()
                if self.parado {
                    self.trava.unlock()
                    Log.captura.notice("fmcam captura: partida cancelada, cliente já saiu")
                    completion(.failure(.camera("Partida cancelada.")))
                    return
                }
                do {
                    let cod = try self.criarCodificador(dim.largura, dim.altura, dim.fps, config.bitrate)
                    cod.aoEvento = { [weak self] ev in self?.aoEvento?(ev) }
                    self.codificador = cod
                    self.trava.unlock()
                    self.motor.definirConsumidor(
                        dono: self,
                        amostra: { [weak self] a in self?.codificarAtual(a) },
                        aoFalha: { [weak self] m in self?.aoEvento?(.erro(m)) },
                        aoReconfigurar: { [weak self] d in self?.reconfigurou(d) })
                    completion(.success(dim))
                } catch let e as ErroFonte {
                    self.trava.unlock()
                    completion(.failure(e))
                } catch {
                    self.trava.unlock()
                    completion(.failure(.camera("Falha ao iniciar a câmera.")))
                }
            }
        }
    }

    /// Qualidade trocada no iPhone durante a conexão. Só o bitrate: ajusta o codificador, sem tipo 1 novo.
    /// Formato novo: reconfigura a câmera e recria o codificador, que manda tipo 1 e quadro-chave (mesmo caminho da lente).
    func reconfigurar(_ config: ConfigVideo, completion: @escaping (Result<DimensoesVideo, ErroFonte>) -> Void) {
        trava.lock()
        // `preset` é o último formato PEDIDO (não o último que terminou): se a troca anterior ainda anda e este pedido volta
        // ao formato de antes, ele precisa ir para a câmera também.
        let presetAntes = preset
        let cod = codificador
        let fechada = parado
        if !fechada, cod != nil { bitrate = config.bitrate; preset = config.preset }
        trava.unlock()
        guard !fechada, let cod else { completion(.failure(.camera("Conexão encerrada."))); return }
        if config.preset == presetAntes {
            cod.ajustarBitrate(config.bitrate)
            completion(.success(DimensoesVideo(largura: cod.largura, altura: cod.altura, fps: cod.fps)))
            return
        }
        motor.mudarPreset(config.preset) { [weak self] r in
            guard let self else { return }
            switch r {
            case .failure(let e):
                self.trava.lock()
                if self.preset == config.preset { self.preset = presetAntes }
                self.trava.unlock()
                completion(.failure(e))
            case .success(let dim):
                self.reconfigurou(dim)
                completion(.success(dim))
            }
        }
    }

    /// Sempre o codificador de agora: ele é trocado quando a lente muda para outro tamanho.
    private func codificarAtual(_ a: CMSampleBuffer) {
        trava.lock(); let cod = codificador; trava.unlock()
        cod?.codificar(a)
    }

    private var estaParado: Bool { trava.lock(); defer { trava.unlock() }; return parado }

    /// Trocou a lente ao vivo: reenvia a config (tipo 1) e força quadro-chave. Se o tamanho mudou, recria o codificador.
    private func reconfigurou(_ dim: DimensoesVideo) {
        trava.lock()
        guard !parado, let atual = codificador else { trava.unlock(); return }
        if atual.largura == dim.largura, atual.altura == dim.altura, atual.fps == dim.fps {
            trava.unlock()
            atual.recomecarConfigEChave()
            return
        }
        do {
            let novo = try criarCodificador(dim.largura, dim.altura, dim.fps, bitrate)
            novo.aoEvento = { [weak self] ev in self?.aoEvento?(ev) }
            codificador = novo
            trava.unlock()
            // O que o codificador velho ainda segura é do tamanho antigo: soltar isso depois do tipo 1 novo faria o
            // decodificador do OBS receber quadro de referência do formato errado.
            atual.aoEvento = nil
            atual.finalizar()
            aoEvento?(.dimensao(dim))
        } catch {
            trava.unlock()
            aoEvento?(.erro("Falha ao recriar o codificador."))
        }
    }

    func parar() {
        trava.lock()
        parado = true
        let cod = codificador
        codificador = nil
        trava.unlock()
        motor.liberarConsumidor(dono: self)
        cod?.finalizar()
    }

    func pedirKeyframe() {
        trava.lock(); let cod = codificador; trava.unlock()
        cod?.pedirKeyframe()
    }

    /// Entrega o que o codificador ainda segura (usado nos testes, que não têm câmera real).
    func esvaziarCodificador() {
        trava.lock(); let cod = codificador; trava.unlock()
        cod?.esvaziar()
    }
}
