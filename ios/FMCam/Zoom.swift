import Foundation

/// Troca de lente por zoom no dispositivo virtual (tripla ou dupla grande-angular): a lente é só um fator de `videoZoomFactor`
/// e o sistema escolhe a câmera física (inclusive caindo para a mais curta quando a longa não foca ou falta luz).
struct MapaZoom: Equatable {
    /// Câmeras físicas do dispositivo virtual, da mais aberta para a mais fechada.
    let constituintes: [Lente]
    /// Fatores de `videoZoomFactor` em que o sistema passa de uma câmera para a próxima (um a menos que os constituintes).
    let trocas: [Double]

    static func ordem(_ l: Lente) -> Int {
        switch l { case .ultra: return 0; case .wide: return 1; case .tele: return 2 }
    }

    init(constituintes: [Lente], trocas: [Double]) {
        self.constituintes = constituintes.sorted { Self.ordem($0) < Self.ordem($1) }
        self.trocas = trocas
    }

    /// Fator de zoom em que a lente aparece pura. A primeira câmera é o fator 1, as outras vêm dos pontos de troca.
    /// nil se o dispositivo não tem essa lente ou se faltam pontos de troca.
    func fatorBase(_ l: Lente) -> Double? {
        guard let i = constituintes.firstIndex(of: l) else { return nil }
        if i == 0 { return 1 }
        guard i - 1 < trocas.count else { return nil }
        return trocas[i - 1]
    }

    /// Fator final para a lente com um zoom digital em cima (1 = lente pura).
    func fator(lente: Lente, zoom: Double) -> Double? {
        fatorBase(lente).map { $0 * max(zoom, 1) }
    }
}

enum RampaZoom {
    /// Transição curta, como a da câmera nativa.
    static let duracaoPadrao = 0.25

    /// `ramp(toVideoZoomFactor:withRate:)` usa potências de dois por segundo: a taxa para durar `duracao` segundos.
    /// nil quando não há o que andar.
    static func taxa(de: Double, para: Double, duracao: Double = duracaoPadrao) -> Float? {
        guard de > 0, para > 0, duracao > 0 else { return nil }
        let passos = abs(log2(para / de))
        guard passos > 0.001 else { return nil }
        return Float(passos / duracao)
    }
}

/// Qual câmera usar para um formato: o dispositivo virtual (troca por zoom) ou a câmera física da lente (troca de entrada).
enum ModoCamera: Equatable {
    case virtual
    case fisica(motivo: String)

    /// O virtual só vale se tem o formato pedido e, quando a estabilização está ligada, se esse formato aceita estabilização.
    static func escolher(virtualExiste: Bool, virtualTemFormato: Bool, estabilizar: Bool, virtualEstabiliza: Bool) -> ModoCamera {
        if !virtualExiste { return .fisica(motivo: "sem dispositivo virtual") }
        if !virtualTemFormato { return .fisica(motivo: "formato não existe no dispositivo virtual") }
        if estabilizar && !virtualEstabiliza { return .fisica(motivo: "estabilização não existe nesse formato do dispositivo virtual") }
        return .virtual
    }
}
