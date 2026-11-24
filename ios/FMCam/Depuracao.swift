import SwiftUI
import UIKit
import os

/// Só diagnóstico no aparelho, ligado por FMCAM_DEBUG=1 no ambiente do processo (devicectl process launch
/// --environment-variables). Sem a variável não faz nada. Cada ação chega por notificação Darwin:
///   devicectl device notification post --device <id> --name fmcam.d.<acao>
/// e passa pelos mesmos métodos dos botões da tela.
@MainActor
enum Depuracao {
    private static var ligado = false
    private static var contador = 0
    private static var relogio: Relogio?

    final class Relogio: NSObject {
        var quadros = 0
        @objc func bate() { quadros += 1 }
    }

    static func ligar(_ m: ModeloApp) {
        guard ProcessInfo.processInfo.environment["FMCAM_DEBUG"] == "1", !ligado else { return }
        ligado = true
        Log.app.notice("fmcam debug: ligado")
        let rel = Relogio()
        let dl = CADisplayLink(target: rel, selector: #selector(Relogio.bate))
        dl.add(to: .main, forMode: .common)
        relogio = rel
        let acoes: [String: () -> Void] = [
            "snap": { foto(m) },
            "estab": { m.mudar { $0.estabilizar.toggle() } },
            "ae": { m.alternarTrava(exposicao: true) },
            "af": { m.alternarTrava(exposicao: false) },
            "bb": { m.mudar { $0.brancoTravado = true; $0.kelvin = nil } },
            "bb-auto": { m.mudar { $0.brancoTravado = false; $0.kelvin = nil } },
            "bb-5000": { m.mudar { $0.brancoTravado = true; $0.kelvin = 5000 } },
            "mf": { m.mudar { $0.focoModo = .manual; $0.posicaoLente = 0.9 } },
            "mf-perto": { m.mudar { $0.focoModo = .manual; $0.posicaoLente = 0.05 } },
            "mf-auto": { m.mudar { $0.focoModo = .auto } },
            "ev+": { m.mudar { $0.bias = 1.5 } },
            "ev0": { m.mudar { $0.bias = 0 } },
            "q1080": { m.mudar { $0.preset = .p1080_30 } },
            "q2160": { m.mudar { $0.preset = .p2160_30 } },
            "grupo": { m.alternarGrupo(.camera) },
            "zerar": { m.mudar { $0 = AjustesCamera() } },
            "estado": { m.captura.registrarEstadoEfetivo(); preview(m) },
            "gira-p": { girar(m, 90, .portrait) },
            "gira-ll": { girar(m, 0, .landscapeLeft) },
            "gira-lr": { girar(m, 180, .landscapeRight) },
        ]
        let centro = CFNotificationCenterGetDarwinNotifyCenter()
        for (nome, acao) in acoes {
            let caixa = Unmanaged.passRetained(Caixa(acao)).toOpaque()
            CFNotificationCenterAddObserver(centro, caixa, { _, obs, _, _, _ in
                guard let obs else { return }
                let c = Unmanaged<Caixa>.fromOpaque(obs).takeUnretainedValue()
                DispatchQueue.main.async { MainActor.assumeIsolated { c.acao() } }
            }, "fmcam.d.\(nome)" as CFString, nil, .deliverImmediately)
        }
    }

    static func layout(_ t: String) {
        guard ProcessInfo.processInfo.environment["FMCAM_DEBUG"] == "1" else { return }
        Log.app.notice("fmcam debug: layout \(t, privacy: .public)")
    }

    private final class Caixa {
        let acao: () -> Void
        init(_ a: @escaping () -> Void) { acao = a }
    }

    /// Gira a interface (cena) e o vídeo como se o aparelho tivesse girado de verdade.
    private static func girar(_ m: ModeloApp, _ angulo: Int, _ o: UIInterfaceOrientationMask) {
        Log.app.notice("fmcam debug: girando para \(angulo, privacy: .public) graus")
        if let cena = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first {
            cena.requestGeometryUpdate(.iOS(interfaceOrientations: o)) { e in
                Log.app.notice("fmcam debug: requestGeometryUpdate erro \(e.localizedDescription, privacy: .public)")
            }
        }
        m.captura.monitorOrientacao.forcar(angulo)
    }

    /// Ângulo que a camada do preview está usando de verdade (para conferir se acompanha o vídeo enviado).
    private static func preview(_ m: ModeloApp) {
        guard let janela = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first?.windows.first else { return }
        func achar(_ v: UIView) -> VistaPreview? {
            if let p = v as? VistaPreview { return p }
            for s in v.subviews { if let p = achar(s) { return p } }
            return nil
        }
        guard let p = achar(janela) else { return }
        var ang = -1.0
        if #available(iOS 17.0, *), let c = p.camada.connection { ang = Double(c.videoRotationAngle) }
        Log.app.notice("fmcam debug: preview angulo=\(ang, privacy: .public) esperado=\(m.anguloFisico, privacy: .public) conexao=\(p.camada.connection != nil, privacy: .public)")
    }

    /// PNG da janela inteira (controles e layout) em Documents, e uma linha de log com o tamanho da janela e as áreas seguras.
    private static func foto(_ m: ModeloApp) {
        guard let cena = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first,
              let janela = cena.windows.first else { return }
        contador += 1
        let r = UIGraphicsImageRenderer(size: janela.bounds.size)
        let img = r.image { _ in janela.drawHierarchy(in: janela.bounds, afterScreenUpdates: true) }
        let doc = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let nome = "snap-\(contador).png"
        try? img.pngData()?.write(to: doc.appendingPathComponent(nome))
        if let v = janela.rootViewController?.view {
            Log.app.notice("fmcam debug: raiz \(NSCoder.string(for: v.frame), privacy: .public) tipo=\(String(describing: type(of: janela.rootViewController!)), privacy: .public) filhos=\(v.subviews.map { NSCoder.string(for: $0.frame) + String(describing: type(of: $0)) }.joined(separator: " | "), privacy: .public)")
        }
        Log.app.notice("fmcam debug: quadros de tela até agora \(relogio?.quadros ?? -1, privacy: .public) ativo=\(UIApplication.shared.applicationState.rawValue, privacy: .public) tela=\(cena.activationState.rawValue, privacy: .public)")
        let ins = janela.safeAreaInsets
        Log.app.notice("fmcam debug: \(nome, privacy: .public) janela \(Int(janela.bounds.width), privacy: .public)x\(Int(janela.bounds.height), privacy: .public) interface=\(cena.interfaceOrientation.rawValue, privacy: .public) seguras t\(Int(ins.top), privacy: .public) b\(Int(ins.bottom), privacy: .public) l\(Int(ins.left), privacy: .public) r\(Int(ins.right), privacy: .public) anguloFisico=\(m.anguloFisico, privacy: .public)")
    }
}
