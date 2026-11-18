import AppIntents
import Foundation

/// Ação do app Atalhos: dias inteiros até a assinatura do app vencer (mesma fonte da validade na tela).
/// Serve para a automação "se <= 2, rodar Refresh All Apps do SideStore".
@available(iOS 16.0, *)
struct DiasParaVencerIntent: AppIntent {
    static var title: LocalizedStringResource = "Dias para vencer"
    static var description = IntentDescription("Quantos dias inteiros faltam para a assinatura do OBS Cam Mobile vencer.")
    static var openAppWhenRun = false

    struct ErroValidade: Error, CustomLocalizedStringResourceConvertible {
        var localizedStringResource: LocalizedStringResource { "Não consegui ler a validade do app." }
    }

    func perform() async throws -> some IntentResult & ReturnsValue<Int> {
        guard let vence = Assinatura.doBundle() else { throw ErroValidade() }
        return .result(value: Assinatura.diasRestantes(vence: vence, agora: Date()))
    }
}

@available(iOS 16.0, *)
struct FMCamAtalhos: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: DiasParaVencerIntent(),
                    phrases: ["Dias para vencer no \(.applicationName)"],
                    shortTitle: "Dias para vencer",
                    systemImageName: "calendar")
    }
}
