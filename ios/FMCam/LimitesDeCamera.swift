import CoreGraphics
import Foundation

/// O AVFoundation lança NSException (que o Swift não pega e derruba o app) quando um setter de câmera recebe valor fora
/// da faixa do dispositivo de agora ou um recurso que ele não tem. Tudo que vai para `AVCaptureDevice` passa por aqui:
/// valor finito, dentro de [min, max] do dispositivo ATUAL (os salvos podem ser de outro aparelho, lente ou formato).
enum LimitesDeCamera {
    /// `v` limitado a [lo, hi]. Não finito (NaN, infinito) cai em `padrao`, também limitado. Faixa invertida vale `lo`.
    static func limitar(_ v: Double, _ lo: Double, _ hi: Double, padrao: Double) -> Double {
        guard lo.isFinite else { return padrao.isFinite ? padrao : 0 }
        let teto = hi.isFinite && hi >= lo ? hi : lo
        let base = v.isFinite ? v : (padrao.isFinite ? padrao : lo)
        return min(max(base, lo), teto)
    }

    static func limitarF(_ v: Float, _ lo: Float, _ hi: Float, padrao: Float) -> Float {
        Float(limitar(Double(v), Double(lo), Double(hi), padrao: Double(padrao)))
    }

    struct Ganhos: Equatable {
        var r: Float, g: Float, b: Float
    }

    /// Ganhos de branco válidos (cada um em 1...max). nil se o dispositivo não informa um máximo válido (antes de a
    /// sessão rodar, ou câmera sem o recurso): aí não há ganho que se possa pedir.
    static func ganhosSeguros(r: Float, g: Float, b: Float, max m: Float) -> Ganhos? {
        guard m.isFinite, m >= 1 else { return nil }
        return Ganhos(r: limitarF(r, 1, m, padrao: 1), g: limitarF(g, 1, m, padrao: 1), b: limitarF(b, 1, m, padrao: 1))
    }

    /// Temperatura de cor aceitável para pedir ganhos ao dispositivo.
    static func kelvinSeguro(_ k: Double) -> Float {
        Float(limitar(k, AjustesCamera.faixaKelvin.lowerBound, AjustesCamera.faixaKelvin.upperBound, padrao: 5500))
    }

    /// Duração de exposição em segundos dentro do que o formato aceita. O teto também respeita a duração do quadro, mas
    /// nunca fica abaixo do piso do formato (o que daria valor fora da faixa).
    static func obturador(_ seg: Double, min lo: Double, max hi: Double, duracaoQuadro: Double, padrao: Double) -> Double {
        let teto = duracaoQuadro.isFinite && duracaoQuadro > 0 ? min(hi, duracaoQuadro) : hi
        return limitar(seg, lo, Swift.max(lo, teto), padrao: padrao)
    }

    /// Zoom dentro do que o dispositivo aceita agora.
    static func zoom(_ z: Double, min lo: Double, max hi: Double) -> Double {
        limitar(z, Swift.max(lo, 1), hi, padrao: 1)
    }

    /// Ponto de interesse de foco e exposição, sempre dentro de 0...1.
    static func ponto(_ p: CGPoint) -> CGPoint {
        CGPoint(x: limitar(Double(p.x), 0, 1, padrao: 0.5), y: limitar(Double(p.y), 0, 1, padrao: 0.5))
    }
}

/// O que fazer com a trava de balanço de branco, conforme o que o dispositivo de agora suporta.
enum PlanoBranco: Equatable {
    case ganhos(LimitesDeCamera.Ganhos)  // setWhiteBalanceModeLocked(with:)
    case modoTravado                     // whiteBalanceMode = .locked
    case ignorar(String)                 // não dá para travar: só log

    static func decidir(suportaGanhos: Bool, suportaTravado: Bool, ganhos: LimitesDeCamera.Ganhos?) -> PlanoBranco {
        if suportaGanhos, let g = ganhos { return .ganhos(g) }
        if suportaTravado { return .modoTravado }
        return .ignorar(suportaGanhos ? "ganhos de branco inválidos neste dispositivo" : "este dispositivo não trava o balanço de branco")
    }
}

/// Uma configuração salva que derrubou o app não pode prender o usuário: toda aplicação de ajustes na câmera marca o
/// início e limpa o fim. Se o app morreu no meio, a marca sobra e a próxima abertura volta sem travas nem manuais.
struct ProtecaoDeCrash {
    static let chave = "aplicandoAjustesDeCamera"
    let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    func marcar() { defaults.set(true, forKey: Self.chave) }
    func limpar() { defaults.removeObject(forKey: Self.chave) }

    /// true uma vez: a marca sobrou da abertura anterior (e é apagada).
    func consumir() -> Bool {
        let sobrou = defaults.bool(forKey: Self.chave)
        if sobrou { limpar() }
        return sobrou
    }
}

extension AjustesCamera {
    /// Sem travas, sem foco e exposição manuais, sem temperatura fixa e sem zoom: o que o iPhone faz sozinho.
    /// Qualidade, lente, guias e estabilização ficam como o dono deixou.
    func semTravas() -> AjustesCamera {
        var a = self
        a.focoTravado = false
        a.exposicaoTravada = false
        a.brancoTravado = false
        a.kelvin = nil
        a.focoModo = .auto
        a.exposicaoModo = .auto
        a.bias = 0
        a.zoom = 1
        return a
    }
}
