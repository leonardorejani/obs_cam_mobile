import Foundation

/// Nível horizontal a partir do vetor de gravidade do aparelho (CoreMotion, eixos do aparelho).
enum Nivel {
    /// Inclinação do horizonte em graus no sentido horário, com o aparelho em paisagem (esquerda ou direita).
    /// nil quando ele está em pé ou deitado (a gravidade quase não cai no eixo x).
    static func angulo(gx: Double, gy: Double) -> Double? {
        let horizontal = abs(gx)
        guard horizontal > 0.5 else { return nil }
        let t = atan2(gy, horizontal) * 180 / .pi
        // com gx negativo o topo do aparelho aponta para a esquerda, com gx positivo para a direita
        return gx < 0 ? t : -t
    }

    /// Inclinação do horizonte nas quatro posições. Em paisagem é `angulo(gx:gy:)`, sem mudar nada. Em pé (a gravidade cai
    /// mais no eixo y) o horizonte corre pelo eixo x. Deitado na mesa ou na diagonal sem lado claro: nil.
    static func anguloEmQualquerPosicao(gx: Double, gy: Double) -> Double? {
        if abs(gx) >= abs(gy) { return angulo(gx: gx, gy: gy) }
        let vertical = abs(gy)
        guard vertical > 0.5 else { return nil }
        let t = atan2(gx, vertical) * 180 / .pi
        // De cabeça para cima o topo do aparelho a ir para a direita faz o horizonte da imagem girar para a esquerda.
        return gy < 0 ? -t : t
    }
}

/// "Nivelado" com histerese: entra com menos de 1 grau e só sai com mais de 1,5 grau, para a linha não piscar.
struct Nivelamento: Equatable {
    static let entra = 1.0
    static let sai = 1.5
    private(set) var nivelado = false

    /// Devolve true só no instante em que passou a nivelado (hora do toque de vibração).
    mutating func atualizar(angulo: Double?) -> Bool {
        guard let a = angulo else {
            nivelado = false
            return false
        }
        let antes = nivelado
        if nivelado {
            if abs(a) > Nivelamento.sai { nivelado = false }
        } else if abs(a) < Nivelamento.entra {
            nivelado = true
        }
        return nivelado && !antes
    }
}

/// O que aparece na tela em cada modo.
struct ElementosVisiveis: Equatable {
    var controles: Bool
    var grade: Bool
    var cruz: Bool
    var nivel: Bool
    var tally: Bool
}

enum ModoLimpo {
    /// No modo limpo a assinatura só aparece (como um ponto amarelo no canto) se faltar menos de 1 dia.
    static func pontoDeAssinatura(limpo: Bool, assinatura: TextoAssinatura?) -> Bool {
        limpo && (assinatura?.menosDeUmDia ?? false)
    }

    /// Modo limpo esconde tudo e deixa só grade, cruz e nível. O tally NO AR nunca some.
    static func elementos(limpo: Bool, ajustes a: AjustesCamera, noAr: Bool) -> ElementosVisiveis {
        if limpo { return ElementosVisiveis(controles: false, grade: true, cruz: true, nivel: true, tally: noAr) }
        return ElementosVisiveis(controles: true, grade: a.grade, cruz: a.centro, nivel: a.nivel, tally: noAr)
    }
}
