import SwiftUI

enum AcaoCamera: Equatable {
    case ligar, desligar
}

/// O que fazer com a câmera em cada fase do app.
enum CicloDeVida {
    static func acao(para fase: ScenePhase) -> AcaoCamera? {
        switch fase {
        case .active: return .ligar
        case .background: return .desligar
        // inactive é teclado, sheet, Central de Controle, notificação, banner de ligação: a câmera segue rodando
        case .inactive: return nil
        @unknown default: return nil
        }
    }
}
