import Foundation

struct TextoAssinatura: Equatable {
    var texto: String
    var urgente: Bool
    /// Texto curto para o selo da faixa de status: "7 dias", "1 dia", "< 1 dia", "vencida".
    var selo = ""
    var vencida = false
    /// Falta menos de 1 dia (ou já venceu): no modo limpo vira um ponto amarelo no canto.
    var menosDeUmDia = false
    /// "25/11 às 10:29"
    var quando = ""
}

/// Vencimento da assinatura do app, lido do embedded.mobileprovision (CMS com um plist XML dentro).
enum Assinatura {
    /// Pega o trecho entre "<?xml" e "</plist>" e lê ExpirationDate. Qualquer problema vira nil.
    static func dataDeExpiracao(dados: Data) -> Date? {
        guard let ini = dados.range(of: Data("<?xml".utf8)),
              let fim = dados.range(of: Data("</plist>".utf8), in: ini.lowerBound..<dados.endIndex) else { return nil }
        let plist = dados[ini.lowerBound..<fim.upperBound]
        guard let obj = try? PropertyListSerialization.propertyList(from: Data(plist), options: [], format: nil),
              let dict = obj as? [String: Any] else { return nil }
        return dict["ExpirationDate"] as? Date
    }

    static func dataDeExpiracao(arquivo: URL) -> Date? {
        guard let d = try? Data(contentsOf: arquivo) else { return nil }
        return dataDeExpiracao(dados: d)
    }

    /// nil no simulador e na App Store (não há perfil embutido).
    static func doBundle(_ bundle: Bundle = .main) -> Date? {
        guard let url = bundle.url(forResource: "embedded", withExtension: "mobileprovision") else { return nil }
        return dataDeExpiracao(arquivo: url)
    }

    /// "Assinatura vence em 5 dias (25/11 12:30)". Amarelo (urgente) com 2 dias ou menos.
    static func texto(vence: Date, agora: Date, fuso: TimeZone = .current) -> TextoAssinatura {
        let f = DateFormatter()
        f.locale = Locale(identifier: "pt_BR")
        f.timeZone = fuso
        f.dateFormat = "dd/MM HH:mm"
        let quando = f.string(from: vence)
        f.dateFormat = "dd/MM 'às' HH:mm"
        let quandoLongo = f.string(from: vence)
        let restante = vence.timeIntervalSince(agora)
        if restante <= 0 {
            return TextoAssinatura(texto: "Assinatura vencida em \(quando)", urgente: true, selo: "vencida",
                                   vencida: true, menosDeUmDia: true, quando: quandoLongo)
        }
        // Dias para CIMA, como o SideStore ("7 DAYS" logo após instalar, com 6 d 23 h de verdade). Zero só quando vencida.
        let dias = max(1, Int((restante / 86400).rounded(.up)))
        let prazo = dias == 1 ? "1 dia" : "\(dias) dias"
        return TextoAssinatura(texto: "Assinatura vence em \(prazo) (\(quando))", urgente: restante <= 2 * 86400,
                               selo: prazo, vencida: false, menosDeUmDia: restante < 86400, quando: quandoLongo)
    }

    static func iso(_ d: Date) -> String {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f.string(from: d)
    }
}

extension Assinatura {
    /// Dias inteiros que faltam, arredondando para BAIXO de propósito (lado seguro da automação do SideStore: com 1,9 dia
    /// o atalho já vê 1 e renova). Diferente da pílula e do painel Validade, que arredondam para cima como o SideStore.
    /// 0 se já venceu ou falta menos de 1 dia.
    static func diasRestantes(vence: Date, agora: Date) -> Int {
        let restante = vence.timeIntervalSince(agora)
        guard restante > 0 else { return 0 }
        return Int(restante / 86400)
    }
}
