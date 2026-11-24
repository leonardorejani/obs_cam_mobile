import CoreGraphics
import Foundation

/// Itens da faixa de cima. Cada um liga e desliga na hora e fica salvo no aparelho.
enum ItemTopo: String, CaseIterable, Codable {
    case estado, nome, formato, iso, bateria, temperatura, validade, ip

    var nomeCompleto: String {
        switch self {
        case .estado: return "Estado da conexão"
        case .nome: return "Nome da câmera"
        case .formato: return "Resolução e quadros por segundo"
        case .iso: return "ISO e obturador"
        case .bateria: return "Bateria"
        case .temperatura: return "Temperatura"
        case .validade: return "Validade da assinatura"
        case .ip: return "Endereço Wi-Fi"
        }
    }

    var rotuloCurto: String {
        switch self {
        case .estado: return "Status"
        case .nome: return "Nome"
        case .formato: return "Formato"
        case .iso: return "ISO"
        case .bateria: return "Bateria"
        case .temperatura: return "Temp."
        case .validade: return "Validade"
        case .ip: return "IP"
        }
    }

    var simbolo: String {
        switch self {
        case .estado: return "dot.radiowaves.left.and.right"
        case .nome: return "tag"
        case .formato: return "film"
        case .iso: return "camera.aperture"
        case .bateria: return "battery.100"
        case .temperatura: return "thermometer.medium"
        case .validade: return "calendar"
        case .ip: return "wifi"
        }
    }

    /// Estado e nome ficam na cápsula da esquerda, o resto na da direita.
    var naEsquerda: Bool { self == .estado || self == .nome }

    /// Padrão: ISO e IP desligados.
    static let padrao: Set<ItemTopo> = [.estado, .nome, .formato, .bateria, .temperatura, .validade]
}

enum LadoMenu: String, Codable, CaseIterable {
    case esquerda, direita

    var rotulo: String { self == .esquerda ? "Esquerda" : "Direita" }
    var oposto: LadoMenu { self == .esquerda ? .direita : .esquerda }
}

/// Preferências da interface (não são ajustes de câmera): itens do topo e lado do menu.
struct PreferenciasTela: Equatable, Codable {
    var topo: Set<ItemTopo> = ItemTopo.padrao
    var lado: LadoMenu = .esquerda

    static let chave = "preferenciasTela"

    enum CodingKeys: String, CodingKey { case topo, lado }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        if let nomes = try? c.decodeIfPresent([String].self, forKey: .topo) {
            topo = Set(nomes.compactMap(ItemTopo.init(rawValue:)))
        }
        if let l = try? c.decodeIfPresent(String.self, forKey: .lado), let v = LadoMenu(rawValue: l) { lado = v }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(topo.map(\.rawValue).sorted(), forKey: .topo)
        try c.encode(lado.rawValue, forKey: .lado)
    }

    static func carregar(defaults: UserDefaults = .standard) -> PreferenciasTela {
        guard let d = defaults.data(forKey: chave), let p = try? JSONDecoder().decode(PreferenciasTela.self, from: d) else {
            return PreferenciasTela()
        }
        return p
    }

    func salvar(defaults: UserDefaults = .standard) {
        if let d = try? JSONEncoder().encode(self) { defaults.set(d, forKey: Self.chave) }
    }

    func mostra(_ i: ItemTopo) -> Bool { topo.contains(i) }

    /// Itens da cápsula da esquerda e da direita, na ordem da tela.
    var itensEsquerda: [ItemTopo] { ItemTopo.allCases.filter { $0.naEsquerda && topo.contains($0) } }
    var itensDireita: [ItemTopo] { ItemTopo.allCases.filter { !$0.naEsquerda && topo.contains($0) } }
}

/// Rótulos curtos que mudam com o valor (ficam nos botões do trilho Câmera).
enum Rotulos {
    /// "BB", ou a temperatura de cor quando travada em um valor (ex.: "5.2K").
    static func bb(_ a: AjustesCamera) -> String {
        if a.brancoTravado, let k = a.kelvin { return String(format: "%.1fK", k / 1000) }
        return "BB"
    }
    static func foco(_ a: AjustesCamera) -> String { a.focoModo == .manual ? "MF" : "AF" }
    /// "EV", "ISO" na exposição manual, ou a compensação com sinal (ex.: "+0.3").
    static func ev(_ a: AjustesCamera) -> String {
        if a.exposicaoModo == .manual { return "ISO" }
        if abs(a.bias) < 0.05 { return "EV" }
        return String(format: "%+.1f", a.bias)
    }
    static func bbAtivo(_ a: AjustesCamera) -> Bool { a.brancoTravado }
    static func focoAtivo(_ a: AjustesCamera) -> Bool { a.focoModo == .manual }
    static func evAtivo(_ a: AjustesCamera) -> Bool { a.exposicaoModo == .manual || abs(a.bias) >= 0.05 }

    /// Aviso fixo no meio da tela quando a câmera não está entregando imagem (antes a tela ficava preta ou parada sem dizer
    /// nada). nil com a câmera ligada, ligando ou desligada de propósito (segundo plano).
    static func avisoCamera(_ e: EstadoCaptura) -> String? {
        switch e {
        case .desligada, .ligando, .ligada: return nil
        case .semPermissao: return "Sem acesso à câmera. Libere em Ajustes > OBS Cam Mobile > Câmera."
        case .semCamera: return "Este aparelho não tem câmera traseira."
        case .interrompida: return "O iPhone pausou a câmera (ligação, outro app ou temperatura). Ela volta sozinha."
        case .erro(let m): return m
        }
    }

    /// Temperatura do balanço de branco travado. Sem temperatura escolhida a trava fica no valor de agora, que não é 5500 K
    /// (o número que o painel mostrava): aparece "Atual" até o operador mexer na régua.
    static func temperatura(_ a: AjustesCamera) -> String {
        a.kelvin.map { "\(Int($0)) K" } ?? "Atual"
    }

    /// "1080p · 60"
    static func formato(_ d: DimensoesVideo?) -> String {
        guard let d else { return "-" }
        return "\(min(d.altura, d.largura))p · \(d.fps)"
    }
}

/// Onde o NO AR aparece: dentro do topo, ou como selo centralizado quando o topo some (modo limpo).
enum PosicaoNoAr: Equatable {
    case nenhuma, pilulaNoTopo, seloCentralizado

    static func de(noAr: Bool, limpo: Bool) -> PosicaoNoAr {
        guard noAr else { return .nenhuma }
        return limpo ? .seloCentralizado : .pilulaNoTopo
    }
}

/// Medidas da tela (em pontos), tiradas do protótipo aprovado. Tudo em relação à área do vídeo 16:9.
enum MedidasTela {
    static let borda: CGFloat = 12
    static let topoY: CGFloat = 8
    static let topoAltura: CGFloat = 36
    static let colunasY: CGFloat = 56
    static let larguraMenu: CGFloat = 72
    static let larguraTrilho: CGFloat = 92
    static let raio: CGFloat = 16
    /// onde começa o painel inferior, contando do lado do menu
    static let painelSemTrilho: CGFloat = 86
    static let painelComTrilho: CGFloat = 188
    /// do lado das lentes: 12 de borda + 56 do botão limpar + 8
    static let painelLadoLentes: CGFloat = 76

    /// Recuos do painel inferior (esquerda, direita), conforme o lado do menu e se o trilho está aberto.
    static func recuosDoPainel(lado: LadoMenu, trilhoAberto: Bool) -> (esquerda: CGFloat, direita: CGFloat) {
        let doMenu = trilhoAberto ? painelComTrilho : painelSemTrilho
        return lado == .esquerda ? (doMenu, painelLadoLentes) : (painelLadoLentes, doMenu)
    }

    /// Tela em pé: o layout dos controles muda (paisagem fica como sempre foi).
    static func emPe(tela: CGSize) -> Bool { tela.height > tela.width }
    /// Em pé a faixa de cima tem duas linhas (estado e nome, depois os itens), com a altura sempre reservada.
    static let topoEmPeAltura: CGFloat = topoAltura * 2 + 6
    /// Do topo da tela em pé: respiro do notch ou da ilha, a faixa de cima e a folga até o menu.
    static func topoEmPe(inseTopo: CGFloat) -> CGFloat { max(inseTopo, 20) + topoY }

    /// Proporção (largura / altura) do vídeo em pé ou deitado. O lado vem de quem desenha, não de `dim`: depois de girar,
    /// `dim` só muda quando o codificador troca (mais de meio segundo), e a tela já está no lado novo.
    static func proporcao(_ dim: DimensoesVideo?, emPe: Bool) -> CGFloat {
        guard let d = dim, d.largura > 0, d.altura > 0 else { return emPe ? 9.0 / 16.0 : 16.0 / 9.0 }
        let p = CGFloat(max(d.largura, d.altura)) / CGFloat(min(d.largura, d.altura))
        return emPe ? 1 / p : p
    }

    static let espacoTrilhoEmPe: CGFloat = 6
    static let itemEmPeMinimo: CGFloat = 60
    /// Em pé todos os botões do trilho têm a mesma largura: dividem a linha, e quando não cabem ela rola de lado.
    static func larguraItemEmPe(disponivel: CGFloat, itens: Int) -> CGFloat {
        guard itens > 0 else { return itemEmPeMinimo }
        return max(itemEmPeMinimo, ((disponivel - espacoTrilhoEmPe * CGFloat(itens - 1)) / CGFloat(itens)).rounded(.down))
    }

    /// Retângulo do vídeo na tela inteira (resizeAspect).
    static func areaDoVideo(tela: CGSize, proporcao: CGFloat) -> CGRect {
        guard proporcao > 0, tela.width > 0, tela.height > 0 else { return CGRect(origin: .zero, size: tela) }
        var w = tela.width, h = tela.width / proporcao
        if h > tela.height { h = tela.height; w = h * proporcao }
        return CGRect(x: (tela.width - w) / 2, y: (tela.height - h) / 2, width: w, height: h)
    }
}
