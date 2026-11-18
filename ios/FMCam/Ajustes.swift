import Foundation

enum ModoFoco: String, Codable, CaseIterable {
    case auto, manual

    var rotulo: String { self == .auto ? "Auto" : "Manual" }

    /// "continuo" (valor salvo por versões anteriores) vira Auto, e qualquer valor desconhecido também.
    init(from decoder: Decoder) throws {
        let t = try decoder.singleValueContainer().decode(String.self)
        self = t == "manual" ? .manual : .auto
    }
}

/// O que a câmera deve fazer com o foco, derivado só dos ajustes. Como não guarda estado, reaplicar depois de
/// reconfigurar a sessão, trocar de lente ou mexer na estabilização dá sempre o mesmo resultado.
/// Não existe foco único (`.autoFocus`): o automático é sempre contínuo.
struct PlanoDeFoco: Equatable {
    enum Modo: Equatable { case continuo, travado, manual }
    var modo: Modo
    /// Ao aplicar, volta o ponto de foco e de exposição para o centro.
    var centralizar: Bool
    /// Observa mudança de cena (AVCaptureDeviceSubjectAreaDidChange) para voltar ao centro sozinho.
    var monitorarCena: Bool
    var posicaoLente: Double?

    static func de(_ a: AjustesCamera) -> PlanoDeFoco {
        if a.focoModo == .manual { return PlanoDeFoco(modo: .manual, centralizar: false, monitorarCena: false, posicaoLente: min(max(a.posicaoLente, 0), 1)) }
        if a.focoTravado { return PlanoDeFoco(modo: .travado, centralizar: false, monitorarCena: false, posicaoLente: nil) }
        return PlanoDeFoco(modo: .continuo, centralizar: true, monitorarCena: true, posicaoLente: nil)
    }

    /// Mudou a cena: o ponto de toque deixa de valer e volta ao centro, só no automático destravado.
    static func devolveAoCentro(_ a: AjustesCamera) -> Bool { de(a).modo == .continuo }
}

enum ModoExposicao: String, Codable, CaseIterable {
    case auto, manual
    var rotulo: String { self == .auto ? "Auto" : "Manual" }
}

/// ISO e obturador de agora, lidos da câmera (também no automático).
struct LeituraCamera: Equatable {
    var iso: Int = 0
    var obturadorSegundos: Double = 0
    var posicaoLente: Double = 0

    var obturadorTexto: String { Obturador.texto(obturadorSegundos) }
}

/// Limites do formato ativo, para os controles manuais.
struct LimitesCamera: Equatable {
    var isoMin: Double = 25
    var isoMax: Double = 3200
    var obturadorMin: Double = 1.0 / 8000
    var obturadorMax: Double = 1.0 / 30
}

enum Obturador {
    /// "1/60" para tempos menores que 1 s, "0,5 s" ou "2 s" acima. 0 vira "-".
    static func texto(_ s: Double) -> String {
        guard s > 0, s.isFinite else { return "-" }
        if s < 0.95 { return "1/\(max(1, Int((1 / s).rounded())))" }
        return String(format: "%g s", (s * 10).rounded() / 10).replacingOccurrences(of: ".", with: ",")
    }
}

/// Ajustes da câmera feitos no iPhone. Ficam salvos no aparelho e voltam ao reabrir.
struct AjustesCamera: Equatable, Codable {
    var lente: Lente = .wide
    /// Zoom digital em cima da lente escolhida (1 = sem zoom).
    var zoom: Double = 1
    var focoTravado = false
    var exposicaoTravada = false
    var brancoTravado = false
    /// Temperatura de cor em K quando o branco está travado de propósito. nil = trava no valor de agora.
    var kelvin: Double?
    /// Compensação de exposição em EV.
    var bias: Double = 0
    var focoModo: ModoFoco = .auto
    /// 0 (perto) a 1 (infinito), usado no foco manual.
    var posicaoLente: Double = 0.5
    var exposicaoModo: ModoExposicao = .auto
    var iso: Double = 100
    /// Tempo de exposição em segundos, usado na exposição manual.
    var obturador: Double = 1.0 / 60
    /// Padrão desligado: a estabilização segura quadros e soma atraso.
    var estabilizar = false
    var grade = true
    var centro = false
    /// Linha de nível horizontal no preview (CoreMotion). Só na tela, nunca no vídeo.
    var nivel = false
    /// Qualidade de envio salva no iPhone. Vale quando o OBS não fixa preset nem bitrate no handshake.
    var preset: Preset = .p1080_30
    var bitrate = PedidoHandshake.bitratePadrao

    typealias Mutacao = (inout AjustesCamera) -> Void

    static let chave = "ajustesCamera"
    static let faixaBias = -3.0...3.0
    static let faixaZoom = 1.0...10.0
    static let faixaKelvin = 2000.0...10000.0

    enum CodingKeys: String, CodingKey {
        case lente, zoom, focoTravado, exposicaoTravada, brancoTravado, kelvin, bias, estabilizar, grade, centro
        case focoModo, posicaoLente, exposicaoModo, iso, obturador, nivel, preset, bitrate
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let base = AjustesCamera()
        lente = (try? c.decodeIfPresent(Lente.self, forKey: .lente)) ?? base.lente
        zoom = min(max((try? c.decodeIfPresent(Double.self, forKey: .zoom)) ?? base.zoom, Self.faixaZoom.lowerBound), Self.faixaZoom.upperBound)
        focoTravado = (try? c.decodeIfPresent(Bool.self, forKey: .focoTravado)) ?? base.focoTravado
        exposicaoTravada = (try? c.decodeIfPresent(Bool.self, forKey: .exposicaoTravada)) ?? base.exposicaoTravada
        brancoTravado = (try? c.decodeIfPresent(Bool.self, forKey: .brancoTravado)) ?? base.brancoTravado
        if let k = try? c.decodeIfPresent(Double.self, forKey: .kelvin) {
            kelvin = min(max(k, Self.faixaKelvin.lowerBound), Self.faixaKelvin.upperBound)
        }
        bias = min(max((try? c.decodeIfPresent(Double.self, forKey: .bias)) ?? base.bias, Self.faixaBias.lowerBound), Self.faixaBias.upperBound)
        estabilizar = (try? c.decodeIfPresent(Bool.self, forKey: .estabilizar)) ?? base.estabilizar
        focoModo = (try? c.decodeIfPresent(ModoFoco.self, forKey: .focoModo)) ?? base.focoModo
        posicaoLente = min(max((try? c.decodeIfPresent(Double.self, forKey: .posicaoLente)) ?? base.posicaoLente, 0), 1)
        exposicaoModo = (try? c.decodeIfPresent(ModoExposicao.self, forKey: .exposicaoModo)) ?? base.exposicaoModo
        iso = min(max((try? c.decodeIfPresent(Double.self, forKey: .iso)) ?? base.iso, 10), 12800)
        obturador = min(max((try? c.decodeIfPresent(Double.self, forKey: .obturador)) ?? base.obturador, 1.0 / 32000), 1)
        grade = (try? c.decodeIfPresent(Bool.self, forKey: .grade)) ?? base.grade
        centro = (try? c.decodeIfPresent(Bool.self, forKey: .centro)) ?? base.centro
        nivel = (try? c.decodeIfPresent(Bool.self, forKey: .nivel)) ?? base.nivel
        preset = (try? c.decodeIfPresent(Preset.self, forKey: .preset)) ?? base.preset
        let b = (try? c.decodeIfPresent(Int.self, forKey: .bitrate)) ?? base.bitrate
        bitrate = PedidoHandshake.faixaBitrate.contains(b) ? b : base.bitrate
    }

    static func carregar(defaults: UserDefaults = .standard) -> AjustesCamera {
        guard let d = defaults.data(forKey: chave),
              let a = try? JSONDecoder().decode(AjustesCamera.self, from: d) else { return AjustesCamera() }
        return a
    }

    func salvar(defaults: UserDefaults = .standard) {
        if let d = try? JSONEncoder().encode(self) { defaults.set(d, forKey: Self.chave) }
    }

    /// Regra de conflito: na conexão o pedido de lente do OBS vale. Mesma lente mantém o zoom, lente diferente zera.
    /// Depois da conexão, o que mudar por último (no iPhone ou no OBS) vale ao vivo.
    /// Só vale o pedido de lente que o cliente fez de propósito. Quem não escreveu `lente` não mexe na escolha do iPhone
    /// (senão cada reconexão do OBS devolvia a câmera para a 1x).
    func aplicando(config: ConfigVideo) -> AjustesCamera {
        config.lenteInformada ? aplicandoPedido(lente: config.lente) : self
    }

    func aplicandoPedido(lente nova: Lente) -> AjustesCamera {
        var a = self
        if a.lente != nova { a.lente = nova; a.zoom = 1 }
        return a
    }
}

/// Estado da câmera lido por outras threads (o servidor manda no status tipo 3).
final class CaixaAjustes {
    private let trava = NSLock()
    private var _valor: AjustesCamera
    private var _leitura = LeituraCamera()
    private var _noAr = false
    private var _assinatura: Date?
    init(_ v: AjustesCamera = AjustesCamera()) { _valor = v }
    var valor: AjustesCamera {
        get { trava.lock(); defer { trava.unlock() }; return _valor }
        set { trava.lock(); _valor = newValue; trava.unlock() }
    }
    var leitura: LeituraCamera {
        get { trava.lock(); defer { trava.unlock() }; return _leitura }
        set { trava.lock(); _leitura = newValue; trava.unlock() }
    }
    /// Quando o perfil de assinatura do app vence (nil se não houver perfil).
    var assinaturaVence: Date? {
        get { trava.lock(); defer { trava.unlock() }; return _assinatura }
        set { trava.lock(); _assinatura = newValue; trava.unlock() }
    }
    var noAr: Bool {
        get { trava.lock(); defer { trava.unlock() }; return _noAr }
        set { trava.lock(); _noAr = newValue; trava.unlock() }
    }
}

// MARK: - Estabilização

enum ModoEstabilizacao: Hashable {
    case standard, previewOptimized
}

enum Estabilizacao {
    /// O de menor atraso entre os suportados. Cinematic e auto não entram: seguram muitos quadros.
    static func escolher(suportados: Set<ModoEstabilizacao>) -> ModoEstabilizacao? {
        if suportados.contains(.standard) { return .standard }
        if suportados.contains(.previewOptimized) { return .previewOptimized }
        return nil
    }
}

// MARK: - Lentes disponíveis

struct InfoLente: Equatable {
    var lente: Lente
    /// Campo de visão horizontal em graus.
    var fov: Double
}

struct OpcaoLente: Equatable, Identifiable {
    var lente: Lente
    var zoom: Double
    var rotulo: String
    var id: String { rotulo }
}

enum OpcoesLente {
    static func fator(fovBase: Double, fov: Double) -> Double {
        tan(fovBase * .pi / 360) / tan(fov * .pi / 360)
    }

    /// 0,5 em 0,5, no jeito brasileiro: "0,5x", "1x", "2,5x".
    static func rotulo(_ f: Double) -> String {
        let r = (f * 2).rounded() / 2
        return (r == r.rounded() ? String(Int(r)) : String(r).replacingOccurrences(of: ".", with: ",")) + "x"
    }

    /// Botões de lente conforme o que o aparelho tem de verdade. O 2x "crop" é zoom na grande-angular
    /// e só aparece quando não há tele curto (menos de 2,75x) para ocupar esse lugar.
    static func montar(_ infos: [InfoLente]) -> [OpcaoLente] {
        guard let wide = infos.first(where: { $0.lente == .wide }) else { return [] }
        var saida: [OpcaoLente] = []
        if let u = infos.first(where: { $0.lente == .ultra }) {
            saida.append(OpcaoLente(lente: .ultra, zoom: 1, rotulo: rotulo(fator(fovBase: wide.fov, fov: u.fov))))
        }
        saida.append(OpcaoLente(lente: .wide, zoom: 1, rotulo: "1x"))
        let tele = infos.first(where: { $0.lente == .tele })
        let fatorTele = tele.map { fator(fovBase: wide.fov, fov: $0.fov) }
        if fatorTele == nil || fatorTele! >= 2.75 {
            saida.append(OpcaoLente(lente: .wide, zoom: 2, rotulo: "2x"))
        }
        if let t = tele, let f = fatorTele {
            saida.append(OpcaoLente(lente: .tele, zoom: 1, rotulo: rotulo(f)))
            _ = t
        }
        return saida
    }

    /// Multiplicador aproximado para mostrar na tela (zoom da lente atual vezes o fator da lente).
    static func fatorAtual(_ a: AjustesCamera, infos: [InfoLente]) -> Double {
        guard let wide = infos.first(where: { $0.lente == .wide }) else { return a.zoom }
        let base = infos.first(where: { $0.lente == a.lente }).map { fator(fovBase: wide.fov, fov: $0.fov) } ?? 1
        return base * a.zoom
    }
}


// MARK: - Qualidade de envio

/// Qualidade que está valendo na conexão e as regras de quem manda nela.
struct QualidadeEnvio: Equatable {
    var preset: Preset
    var bitrate: Int
    /// O OBS fixou o preset nesta conexão (Resolução e Quadros ficam só leitura).
    var presetObs: Bool
    /// O OBS fixou o bitrate nesta conexão.
    var bitrateObs: Bool
    /// Compatibilidade: o OBS fixou preset ou bitrate.
    var escolhidaNoOBS: Bool { presetObs || bitrateObs }

    static let bitrates = [6_000_000, 10_000_000, 15_000_000, 20_000_000, 30_000_000]
    static let formatos: [Preset] = [.p720_30, .p720_60, .p1080_30, .p1080_60, .p1080_120, .p2160_30]

    /// Cada campo é resolvido sozinho: ausente no handshake = vale o salvo no iPhone (e pode mudar no meio da conexão),
    /// presente = vale o do OBS e fica travado nesta conexão.
    static func resolver(pedido: PedidoHandshake, salva: AjustesCamera) -> QualidadeEnvio {
        QualidadeEnvio(preset: pedido.presetInformado ? pedido.preset : salva.preset,
                       bitrate: pedido.bitrateInformado ? pedido.bitrate : salva.bitrate,
                       presetObs: pedido.presetInformado, bitrateObs: pedido.bitrateInformado)
    }

    /// O que vale agora na conexão, dado o que o iPhone salvou: campo travado pelo OBS não muda, o outro acompanha o salvo.
    func comSalva(_ salva: AjustesCamera) -> QualidadeEnvio {
        QualidadeEnvio(preset: presetObs ? preset : salva.preset, bitrate: bitrateObs ? bitrate : salva.bitrate,
                       presetObs: presetObs, bitrateObs: bitrateObs)
    }

    /// Texto curto embaixo do painel, só quando algo está travado.
    static func aviso(presetObs: Bool, bitrateObs: Bool) -> String? {
        switch (presetObs, bitrateObs) {
        case (false, false): return nil
        case (true, false): return "Formato escolhido no OBS."
        case (false, true): return "Bitrate escolhido no OBS."
        case (true, true): return "Formato e bitrate escolhidos no OBS. Para mudar aqui, deixe o OBS em Definida no iPhone."
        }
    }

    /// "10 Mbps", "6,5 Mbps"
    static func rotulo(bitrate: Int) -> String {
        let m = Double(bitrate) / 1_000_000
        if m == m.rounded() { return "\(Int(m)) Mbps" }
        return String(format: "%.1f Mbps", m).replacingOccurrences(of: ".", with: ",")
    }

    /// "6", "10"... (o painel escreve "Mbps" uma vez ao lado, para caber em iPhone pequeno).
    static func numero(bitrate: Int) -> String {
        rotulo(bitrate: bitrate).replacingOccurrences(of: " Mbps", with: "")
    }

    /// Formato que está saindo, a partir das dimensões reais.
    static func preset(de d: DimensoesVideo?) -> Preset? {
        guard let d else { return nil }
        return Preset.allCases.first { $0.altura == d.altura && $0.largura == d.largura && $0.fps == d.fps }
    }
}

// MARK: - Resolução e quadros (dois segmentos que montam o preset do protocolo)

enum Resolucao: String, CaseIterable {
    case r720 = "720p", r1080 = "1080p", r4k = "4K"

    var altura: Int {
        switch self { case .r720: return 720; case .r1080: return 1080; case .r4k: return 2160 }
    }
    static func de(_ p: Preset) -> Resolucao {
        switch p.altura { case 720: return .r720; case 1080: return .r1080; default: return .r4k }
    }
}

extension QualidadeEnvio {
    static let quadros = [30, 60, 120]

    /// O preset do protocolo para a combinação, ou nil se ela não existe (720p120, 4K60, 4K120).
    static func preset(resolucao: Resolucao, fps: Int) -> Preset? {
        Preset.allCases.first { Resolucao.de($0) == resolucao && $0.fps == fps }
    }

    /// Combinação existe no protocolo e a lente aceita.
    static func disponivel(_ r: Resolucao, _ fps: Int, suportados: Set<Preset>) -> Bool {
        guard let p = preset(resolucao: r, fps: fps) else { return false }
        return suportados.contains(p)
    }

    /// Trocou a resolução: mantém o fps se existir, senão cai para o maior fps válido abaixo (ou o menor acima, se não houver abaixo).
    static func trocar(resolucao r: Resolucao, fpsAtual: Int, suportados: Set<Preset>) -> Preset? {
        let validos = quadros.filter { disponivel(r, $0, suportados: suportados) }
        guard !validos.isEmpty else { return nil }
        let fps = validos.filter { $0 <= fpsAtual }.max() ?? validos.min() ?? fpsAtual
        return preset(resolucao: r, fps: fps)
    }
}
