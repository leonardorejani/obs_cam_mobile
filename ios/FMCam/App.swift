import SwiftUI
import UIKit
import os

@main
struct FMCamApp: App {
    @StateObject private var modelo = ModeloApp()
    @Environment(\.scenePhase) private var fase

    var body: some Scene {
        WindowGroup {
            TelaPrincipal(modelo: modelo)
                .preferredColorScheme(.dark)
                .statusBarHidden(true)
                .onAppear {
                    UIApplication.shared.isIdleTimerDisabled = true
                    modelo.iniciar()
                    modelo.primeiroPlano(true)
                }
                .onDisappear { UIApplication.shared.isIdleTimerDisabled = false }
                .onChange(of: fase) { nova in
                    Log.app.notice("fmcam app: scenePhase \(String(describing: nova), privacy: .public)")
                    // Tela sempre acesa e câmera ligada só em primeiro plano.
                    UIApplication.shared.isIdleTimerDisabled = (nova != .background)
                    if let acao = CicloDeVida.acao(para: nova) { modelo.primeiroPlano(acao == .ligar) }
                }
        }
    }
}
