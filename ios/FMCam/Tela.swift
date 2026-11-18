import AVFoundation
import CoreMotion
import Network
import SwiftUI
import UIKit
import os

struct IndicadorFoco: Equatable {
    var ponto: CGPoint
    var travado: Bool
    var id = UUID()
}

@MainActor
final class ModeloApp: ObservableObject {
    @Published var info = InfoServidor()
    @Published var bateria: Double = -1
    @Published var carregando = false
    @Published var temperatura = "Normal"
    @Published var nome: String
    @Published var campoNome: String
    @Published var ajustes: AjustesCamera {
        didSet {
            ajustes.salvar()
            caixaAjustes.valor = ajustes
        }
    }
    @Published var estadoCamera: EstadoCaptura = .desligada
    @Published var lentes: [OpcaoLente] = []
    /// Modo limpo: some tudo e fica só grade, cruz e nível (o tally NO AR continua).
    @Published var limpo = false
    @Published var painel: PainelRapido?
    @Published var grupo: GrupoMenu?
    /// Formatos que a lente atual aceita (os outros ficam desabilitados no painel Qualidade).
    @Published var presetsSuportados: Set<Preset> = Set(Preset.allCases)
    @Published var prefs: PreferenciasTela = PreferenciasTela.carregar() {
        didSet { prefs.salvar() }
    }
    @Published var anguloNivel: Double?
    @Published var nivelado = false
    /// Só no simulador, para o print do layout.
    @Published var noArForcado = false
    @Published var indicadorFoco: IndicadorFoco?
    @Published var ipWifi: String?
    @Published var copiado = false
    @Published var leitura = LeituraCamera()
    @Published var limites = LimitesCamera()
    @Published var dimCamera: DimensoesVideo?
    @Published var aviso: String?
    @Published var assinatura: TextoAssinatura?
    private let vencimento: Date?
    private let motion = CMMotionManager()
    private var nivelamento = Nivelamento()
    private var ativoNoApp = true

    let identidade: IdentidadeAparelho
    let captura = CapturaCompartilhada()
    private let infosLentes: [InfoLente]
    private let idVendor: String
    private let sensores = CaixaSensores()
    private let caixaAjustes: CaixaAjustes
    private var servidor: Servidor?
    private var timer: Timer?
    private var monitor: NWPathMonitor?
    private var zoomBase = 1.0
    private var iniciou = false
    private let emTeste = ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil

    init() {
        UIDevice.current.isBatteryMonitoringEnabled = true
        let id = UIDevice.current.identifierForVendor?.uuidString ?? ""
        idVendor = id
        let n = NomeCamera.ler(idVendor: id)
        nome = n
        campoNome = n
        identidade = IdentidadeAparelho(modelo: ModeloApp.modeloDoAparelho(), nome: n, id: id)
        var venc = emTeste ? nil : Assinatura.doBundle()
        #if targetEnvironment(simulator)
        // Só para o print: simula um perfil que vence daqui a N dias.
        if let d = ProcessInfo.processInfo.environment["FMCAM_PRINT_VENCE_DIAS"], let n = Double(d) {
            venc = Date().addingTimeInterval(n * 86400)
        }
        #endif
        vencimento = venc
        let a = AjustesCamera.carregar()
        ajustes = a
        caixaAjustes = CaixaAjustes(a)
        caixaAjustes.assinaturaVence = venc
        assinatura = venc.map { Assinatura.texto(vence: $0, agora: Date()) }
        #if targetEnvironment(simulator)
        // O simulador não tem câmera: lentes de exemplo só para o layout fazer sentido.
        infosLentes = emTeste ? [] : [InfoLente(lente: .ultra, fov: 120), InfoLente(lente: .wide, fov: 69), InfoLente(lente: .tele, fov: 15.6)]
        #else
        infosLentes = emTeste ? [] : Captura.infosLentes()
        #endif
        lentes = OpcoesLente.montar(infosLentes)
        captura.definirAjustesIniciais(a)
        #if targetEnvironment(simulator)
        let env = ProcessInfo.processInfo.environment
        if env["FMCAM_PRINT_LIMPO"] == "1" { limpo = true }
        if let g = env["FMCAM_PRINT_ANGULO"], let v = Double(g) { anguloNivel = v; nivelado = abs(v) < 1 }
        if env["FMCAM_PRINT_NOAR"] == "1" { noArForcado = true }
        if let p = env["FMCAM_PRINT_PAINEL"] { painel = PainelRapido(rawValue: p) }
        if let g = env["FMCAM_PRINT_GRUPO"] { grupo = GrupoMenu(rawValue: g) }
        // o print não pode herdar preferências de execuções anteriores
        prefs = PreferenciasTela()
        prefs.lado = env["FMCAM_PRINT_LADO"] == "direita" ? .direita : .esquerda
        ajustes = AjustesCamera()
        caixaAjustes.valor = ajustes
        dimCamera = DimensoesVideo(largura: 1920, altura: 1080, fps: 60)
        leitura = LeituraCamera(iso: 483, obturadorSegundos: 1.0 / 60, posicaoLente: 0.4)
        if env["FMCAM_PRINT_QUALIDADE_OBS"] == "1" { info.bitrateObs = true; info.bitrate = 20_000_000 }
        presetsSuportados = Captura.presetsSuportados(lente: ajustes.lente)
        if env["FMCAM_PRINT_AVISO"] == "1" { aviso = "A estabilização não está disponível nesta lente e formato." }
        #endif
        captura.aoLeitura = { [weak self] l in
            self?.caixaAjustes.leitura = l
            Task { @MainActor in self?.leitura = l }
        }
        captura.aoAviso = { [weak self] m in Task { @MainActor in self?.mostrarAviso(m) } }
        captura.aoLimites = { [weak self] l in Task { @MainActor in self?.limites = l } }
        captura.aoDimensao = { [weak self] d in Task { @MainActor in self?.dimCamera = d } }
        captura.aoEstado = { [weak self] e in Task { @MainActor in self?.estadoCamera = e } }
        captura.aoMutacao = { [weak self] m in
            Task { @MainActor in
                guard let self else { return }
                var a = self.ajustes
                m(&a)
                self.ajustes = a
            }
        }
    }

    func iniciar() {
        guard !iniciou else { return }
        iniciou = true
        atualizarSensores()
        atualizarRede()
        atualizarPresetsSuportados()
        Log.app.notice("fmcam app: iniciado, thermalState \(temperaturaTexto(ProcessInfo.processInfo.thermalState), privacy: .public)")
        NotificationCenter.default.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil) { _ in
            Log.app.notice("fmcam app: thermalState mudou para \(temperaturaTexto(ProcessInfo.processInfo.thermalState), privacy: .public)")
        }
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.atualizarSensores() }
        }
        let m = NWPathMonitor()
        m.pathUpdateHandler = { [weak self] _ in Task { @MainActor in self?.atualizarRede() } }
        m.start(queue: DispatchQueue(label: "fmcam.rede"))
        monitor = m
        // Os testes sobem o próprio servidor, o do app fica de fora.
        if emTeste { return }
        let motor = captura
        let s = Servidor(identidade: IdentidadeAparelho(modelo: identidade.modelo, nome: nome, id: identidade.id),
                         sensores: sensores, ajustes: caixaAjustes) { FonteCamera(motor: motor) }
        s.aoAtualizar = { [weak self] i in Task { @MainActor in self?.info = i } }
        servidor = s
        s.iniciar()
    }

    /// A câmera fica ligada sempre que o app está em primeiro plano, mesmo sem cliente.
    func primeiroPlano(_ ativo: Bool) {
        if emTeste { return }
        ativoNoApp = ativo
        if ativo { captura.ligarEmPrimeiroPlano() } else { captura.desligar() }
        atualizarMovimento()
    }

    /// Toast no topo: some em 3 s.
    func mostrarAviso(_ m: String) {
        aviso = m
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            if self.aviso == m { self.aviso = nil }
        }
    }

    // MARK: menu, trilhos e painéis

    func alternarGrupo(_ g: GrupoMenu) {
        if grupo == g { grupo = nil; painel = nil } else { grupo = g; painel = nil }
        Haptico.leve()
    }

    func alternarItemDoTopo(_ i: ItemTopo) {
        if prefs.topo.contains(i) { prefs.topo.remove(i) } else { prefs.topo.insert(i) }
    }

    func definirLado(_ l: LadoMenu) { prefs.lado = l }

    /// Trava de exposição ou de foco, separadas (o toque longo na imagem continua travando as duas juntas).
    func alternarTrava(exposicao: Bool) {
        if exposicao {
            let nova = !ajustes.exposicaoTravada
            mudar { $0.exposicaoTravada = nova }
            mostrarAviso(nova ? "Exposição travada" : "Exposição automática")
        } else {
            let nova = !ajustes.focoTravado
            mudar { $0.focoTravado = nova }
            mostrarAviso(nova ? "Foco travado" : "Foco automático")
        }
        Haptico.medio()
    }

    /// O que o toque no chip de estado mostra.
    var detalheDoEstado: String {
        switch info.estado {
        case .iniciando: return "Iniciando"
        case .esperando: return "Esperando conexão do OBS"
        case .conectado(let c):
            let via = info.origem.map { " (\($0.rawValue))" } ?? ""
            return "Conectado a \(c)\(via)"
        case .erroCamera(let m), .erroRede(let m): return m
        }
    }

    // MARK: modo limpo e nível

    var noArTela: Bool { info.noAr || noArForcado }
    var elementos: ElementosVisiveis { ModoLimpo.elementos(limpo: limpo, ajustes: ajustes, noAr: noArTela) }

    func alternarLimpo() {
        limpo.toggle()
        if limpo { painel = nil }
        atualizarMovimento()
        Haptico.leve()
    }

    func atualizarPresetsSuportados() {
        if emTeste { return }
        presetsSuportados = Captura.presetsSuportados(lente: ajustes.lente)
    }

    /// Cada linha do painel Qualidade trava sozinha: só o que o OBS fixou nesta conexão fica só leitura.
    var formatoTravado: Bool { info.presetObs }
    var bitrateTravado: Bool { info.bitrateObs }

    func alternarPainel(_ p: PainelRapido) {
        if p == .qualidade { atualizarPresetsSuportados() }
        let saindoDoNome = painel == .nome
        painel = (painel == p) ? nil : p
        if saindoDoNome && painel != .nome { salvarNome() }
    }

    /// Motion só roda quando o nível aparece (modo limpo ou nível ligado) e o app está em primeiro plano.
    private func atualizarMovimento() {
        let precisa = (limpo || ajustes.nivel) && ativoNoApp && !emTeste
        if precisa, !motion.isDeviceMotionActive, motion.isDeviceMotionAvailable {
            motion.deviceMotionUpdateInterval = 1.0 / 30
            motion.startDeviceMotionUpdates(to: .main) { [weak self] m, _ in
                guard let g = m?.gravity else { return }
                Task { @MainActor in self?.receberGravidade(gx: g.x, gy: g.y) }
            }
        } else if !precisa, motion.isDeviceMotionActive {
            motion.stopDeviceMotionUpdates()
            anguloNivel = nil
            nivelado = false
            nivelamento = Nivelamento()
        }
    }

    func receberGravidade(gx: Double, gy: Double) {
        let a = Nivel.angulo(gx: gx, gy: gy)
        anguloNivel = a
        let entrou = nivelamento.atualizar(angulo: a)
        nivelado = nivelamento.nivelado
        if entrou { Haptico.leve() }
    }

    /// Toque na imagem: no modo limpo sai dele, fora dele foca.
    func tocarNaImagem(camada: CGPoint, dispositivo: CGPoint, travar: Bool) {
        if limpo { alternarLimpo(); return }
        if painel == .nome { salvarNome() }
        painel = nil
        tocar(camada: camada, dispositivo: dispositivo, travar: travar)
    }

    // MARK: ajustes

    func mudar(_ f: AjustesCamera.Mutacao) {
        var a = ajustes
        f(&a)
        guard a != ajustes else { return }
        let nivelMudou = a.nivel != ajustes.nivel
        let qualidadeMudou = a.preset != ajustes.preset || a.bitrate != ajustes.bitrate
        let lenteMudou = a.lente != ajustes.lente
        ajustes = a
        captura.definirAjustes(a)
        if qualidadeMudou { servidor?.qualidadeMudou() }
        if lenteMudou { atualizarPresetsSuportados() }
        if nivelMudou { atualizarMovimento() }
    }

    func selecionar(_ o: OpcaoLente) {
        mudar { $0.lente = o.lente; $0.zoom = o.zoom }
        Haptico.leve()
    }

    func estaSelecionada(_ o: OpcaoLente) -> Bool {
        ajustes.lente == o.lente && abs(ajustes.zoom - o.zoom) < 0.15
    }

    var multiplicador: String { OpcoesLente.rotulo(OpcoesLente.fatorAtual(ajustes, infos: infosLentes)) }

    func pinca(escala: Double, fase: UIGestureRecognizer.State) {
        if fase == .began { zoomBase = ajustes.zoom }
        let z = min(max(zoomBase * escala, AjustesCamera.faixaZoom.lowerBound), AjustesCamera.faixaZoom.upperBound)
        mudar { $0.zoom = z }
    }

    func tocar(camada: CGPoint, dispositivo: CGPoint, travar: Bool) {
        captura.focar(ponto: dispositivo, travar: travar)
        if travar { Haptico.medio() }
        let i = IndicadorFoco(ponto: camada, travado: travar)
        indicadorFoco = i
        if !travar {
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                if self.indicadorFoco?.id == i.id { self.indicadorFoco = nil }
            }
        }
    }

    func salvarNome() {
        nome = NomeCamera.salvar(campoNome, idVendor: idVendor)
        campoNome = nome
        servidor?.atualizarNome(nome)
    }

    // MARK: rede e sensores

    var enderecoTexto: String { EnderecoRede.formatar(ip: ipWifi, porta: Protocolo.portaPadrao) }

    func copiarEndereco() {
        guard let e = EnderecoRede.enderecoParaCopiar(ip: ipWifi, porta: Protocolo.portaPadrao) else { return }
        UIPasteboard.general.string = e
        copiado = true
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            self.copiado = false
        }
    }

    private func atualizarRede() { ipWifi = EnderecoRede.escolherWifi(EnderecoRede.listar()) }

    private func atualizarSensores() {
        let d = UIDevice.current
        #if targetEnvironment(simulator)
        let nivel = d.batteryLevel < 0 ? 0.95 : Double(d.batteryLevel)
        #else
        let nivel = Double(d.batteryLevel)
        #endif
        bateria = nivel
        carregando = d.batteryState == .charging || d.batteryState == .full
        sensores.atualizar(bateria: nivel, carregando: carregando)
        assinatura = vencimento.map { Assinatura.texto(vence: $0, agora: Date()) }
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: temperatura = "Normal"
        case .fair: temperatura = "Morno"
        case .serious: temperatura = "Quente"
        case .critical: temperatura = "Crítica"
        @unknown default: temperatura = "Normal"
        }
    }

    static func modeloDoAparelho() -> String {
        if let sim = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return sim }
        var u = utsname()
        uname(&u)
        return withUnsafePointer(to: &u.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(_SYS_NAMELEN)) { String(cString: $0) }
        }
    }
}

// MARK: - Haptic

enum Haptico {
    static func leve() { UIImpactFeedbackGenerator(style: .light).impactOccurred() }
    static func medio() { UIImpactFeedbackGenerator(style: .medium).impactOccurred() }
}

// MARK: - Preview

final class VistaPreview: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var camada: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }

    var aoTocar: ((CGPoint, CGPoint, Bool) -> Void)?
    var aoToqueDuplo: (() -> Void)?
    var aoPinca: ((Double, UIGestureRecognizer.State) -> Void)?
    private var observadores: [NSObjectProtocol] = []

    override init(frame: CGRect) {
        super.init(frame: frame)
        #if targetEnvironment(simulator)
        backgroundColor = .clear
        #else
        backgroundColor = .black
        #endif
        // Mostra exatamente o quadro que vai para o OBS, sem corte.
        camada.videoGravity = .resizeAspect

        let duplo = UITapGestureRecognizer(target: self, action: #selector(toqueDuplo))
        duplo.numberOfTapsRequired = 2
        let simples = UITapGestureRecognizer(target: self, action: #selector(toqueSimples(_:)))
        simples.require(toFail: duplo)
        let longo = UILongPressGestureRecognizer(target: self, action: #selector(toqueLongo(_:)))
        longo.minimumPressDuration = 0.6
        let pinca = UIPinchGestureRecognizer(target: self, action: #selector(pincou(_:)))
        [duplo, simples, longo, pinca].forEach(addGestureRecognizer)

        let c = NotificationCenter.default
        observadores.append(c.addObserver(forName: .AVCaptureSessionDidStartRunning, object: nil, queue: .main) { [weak self] _ in
            self?.aplicarOrientacao()
        })
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        observadores.append(c.addObserver(forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.aplicarOrientacao()
        })
    }

    required init?(coder: NSCoder) { fatalError() }

    deinit {
        observadores.forEach(NotificationCenter.default.removeObserver)
        UIDevice.current.endGeneratingDeviceOrientationNotifications()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        aplicarOrientacao()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        aplicarOrientacao()
    }

    /// Preview na mesma orientação do vídeo enviado (paisagem do sensor), de pé nas duas paisagens.
    func aplicarOrientacao() {
        guard let conn = camada.connection else { return }
        let o = window?.windowScene?.interfaceOrientation ?? .landscapeRight
        if #available(iOS 17.0, *) {
            let angulo: CGFloat = (o == .landscapeLeft) ? 180 : 0
            if conn.isVideoRotationAngleSupported(angulo) { conn.videoRotationAngle = angulo }
        } else if conn.isVideoOrientationSupported {
            conn.videoOrientation = (o == .landscapeLeft) ? .landscapeLeft : .landscapeRight
        }
    }

    private func pontoDoDispositivo(_ p: CGPoint) -> CGPoint {
        let d = camada.captureDevicePointConverted(fromLayerPoint: p)
        return CGPoint(x: min(max(d.x, 0), 1), y: min(max(d.y, 0), 1))
    }

    @objc private func toqueSimples(_ g: UITapGestureRecognizer) {
        let p = g.location(in: self)
        aoTocar?(p, pontoDoDispositivo(p), false)
    }
    @objc private func toqueLongo(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began else { return }
        let p = g.location(in: self)
        aoTocar?(p, pontoDoDispositivo(p), true)
    }
    @objc private func toqueDuplo() { aoToqueDuplo?() }
    @objc private func pincou(_ g: UIPinchGestureRecognizer) { aoPinca?(Double(g.scale), g.state) }
}

struct PreviewCamera: UIViewRepresentable {
    let sessao: AVCaptureSession
    var aoTocar: (CGPoint, CGPoint, Bool) -> Void
    var aoToqueDuplo: () -> Void
    var aoPinca: (Double, UIGestureRecognizer.State) -> Void

    func makeUIView(context: Context) -> VistaPreview {
        let v = VistaPreview()
        v.camada.session = sessao
        return v
    }

    func updateUIView(_ v: VistaPreview, context: Context) {
        v.aoTocar = aoTocar
        v.aoToqueDuplo = aoToqueDuplo
        v.aoPinca = aoPinca
    }
}

// MARK: - Visual

extension Color {
    /// Amarelo de câmera: estados ativos e travados.
    static let destaque = Color(red: 1.0, green: 0.839, blue: 0.039)
}

enum PainelRapido: String {
    case exposicao, branco, foco, nome, qualidade, wifi, validade, gestos, lado
}

enum GrupoMenu: String, CaseIterable {
    case camera, guias, info, ajustes

    var rotulo: String {
        switch self { case .camera: return "Câmera"; case .guias: return "Guias"; case .info: return "Info"; case .ajustes: return "Ajustes" }
    }
    var titulo: String { rotulo.uppercased() }
    var simbolo: String {
        switch self { case .camera: return "camera"; case .guias: return "grid"; case .info: return "info.circle"; case .ajustes: return "gearshape" }
    }
}

/// Fundo dos módulos: preto translúcido com blur, raio 16 e borda branca de 8%.
struct Vidro: ViewModifier {
    var raio: CGFloat = MedidasTela.raio

    func body(content: Content) -> some View {
        let forma = RoundedRectangle(cornerRadius: raio, style: .continuous)
        return content
            .background(ZStack {
                forma.fill(.ultraThinMaterial)
                forma.fill(Color(red: 0.07, green: 0.063, blue: 0.094).opacity(0.62))
            })
            .overlay(forma.stroke(Color.white.opacity(0.08), lineWidth: 1))
    }
}

extension View {
    func vidro(_ raio: CGFloat = MedidasTela.raio) -> some View { modifier(Vidro(raio: raio)) }
}

/// Pressionado: escala e opacidade em 150 ms (sem animação com Reduzir Movimento).
struct EstiloPressionar: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) private var reduzir

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.95 : 1)
            .opacity(configuration.isPressed ? 0.8 : 1)
            .animation(reduzir ? nil : .easeOut(duration: 0.15), value: configuration.isPressed)
    }
}

/// Botão de largura total do trilho: ícone em destaque e nome curto. Ativo é amarelo, aberto ganha contorno branco.
struct BotaoTrilho: View {
    let simbolo: String
    let rotulo: String
    var ativo = false
    var aberto = false
    let descricao: String
    let acao: () -> Void

    var body: some View {
        let forma = RoundedRectangle(cornerRadius: 12, style: .continuous)
        return Button(action: acao) {
            VStack(spacing: 4) {
                Image(systemName: simbolo).font(.system(size: 19, weight: .semibold))
                Text(rotulo).font(.system(size: 10, weight: .semibold)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity, minHeight: 48)
            .foregroundColor(ativo ? .black : .white)
            .background(forma.fill(ativo ? Color.destaque : Color.white.opacity(0.06)))
            .overlay(forma.stroke(Color.white, lineWidth: aberto ? 2 : 0))
            .contentShape(forma)
        }
        .buttonStyle(EstiloPressionar())
        .accessibilityLabel(descricao)
        .accessibilityAddTraits(ativo ? .isSelected : [])
    }
}

struct ItemDoMenu: View {
    let grupo: GrupoMenu
    let selecionado: Bool
    let acao: () -> Void

    var body: some View {
        let forma = RoundedRectangle(cornerRadius: 12, style: .continuous)
        return Button(action: acao) {
            VStack(spacing: 4) {
                Image(systemName: grupo.simbolo).font(.system(size: 20, weight: .semibold))
                    .foregroundColor(selecionado ? .destaque : .white.opacity(0.75))
                Text(grupo.rotulo).font(.system(size: 10, weight: .semibold))
                    .foregroundColor(selecionado ? .white : .white.opacity(0.75)).lineLimit(1)
            }
            .frame(maxWidth: .infinity, minHeight: 56)
            .background(forma.fill(selecionado ? Color.white.opacity(0.14) : .clear))
            .contentShape(forma)
        }
        .buttonStyle(EstiloPressionar())
        .accessibilityLabel("Menu \(grupo.rotulo)")
        .accessibilityAddTraits(selecionado ? .isSelected : [])
    }
}

/// Segmento de opções (Auto | Manual). A largura acompanha o texto, nunca corta.
struct Segmento: View {
    let opcoes: [String]
    let atual: String
    var desabilitados: Set<String> = []
    var compacto = false
    let acao: (String) -> Void

    var body: some View {
        HStack(spacing: 2) {
            ForEach(opcoes, id: \.self) { o in
                Button { acao(o) } label: {
                    Text(o).font(.system(size: compacto ? 12 : 12.5, weight: .semibold)).lineLimit(1).fixedSize()
                        .padding(.horizontal, compacto ? 9 : 14).frame(minHeight: 36)
                        .foregroundColor(o == atual ? .black : .white)
                        .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(o == atual ? Color.destaque : .clear))
                }
                .buttonStyle(EstiloPressionar())
                .disabled(desabilitados.contains(o))
                .opacity(desabilitados.contains(o) ? 0.35 : 1)
                .accessibilityAddTraits(o == atual ? .isSelected : [])
            }
        }
        .padding(3)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.08)))
        .fixedSize()
    }
}

struct Regua: View {
    let rotulo: String
    @Binding var valor: Double
    let faixa: ClosedRange<Double>
    var passo: Double?
    let texto: String
    var rotuloFim: String?

    var body: some View {
        HStack(spacing: 10) {
            Text(rotulo).font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).fixedSize()
            Group {
                if let p = passo { Slider(value: $valor, in: faixa, step: p) } else { Slider(value: $valor, in: faixa) }
            }
            .tint(.destaque)
            .accessibilityLabel(rotulo)
            if let f = rotuloFim { Text(f).font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).fixedSize() }
            Text(verbatim: texto).font(.system(size: 12, weight: .bold)).monospacedDigit().foregroundColor(.white)
                .frame(minWidth: 54, alignment: .trailing).fixedSize()
        }
    }
}

/// Guias só do preview (nunca entram no vídeo): grade de terços, cruz central e nível horizontal.
struct GuiasView: View {
    var grade: Bool
    var cruz: Bool
    var nivel: Bool
    var angulo: Double?
    var nivelado: Bool
    var proporcao: CGFloat
    @Environment(\.accessibilityReduceMotion) private var reduzir

    var body: some View {
        GeometryReader { g in
            let area = ajustar(g.size)
            ZStack {
                Path { p in
                    guard grade else { return }
                    for i in 1...2 {
                        let x = area.minX + area.width * CGFloat(i) / 3
                        let y = area.minY + area.height * CGFloat(i) / 3
                        p.move(to: CGPoint(x: x, y: area.minY)); p.addLine(to: CGPoint(x: x, y: area.maxY))
                        p.move(to: CGPoint(x: area.minX, y: y)); p.addLine(to: CGPoint(x: area.maxX, y: y))
                    }
                }
                .stroke(Color.white.opacity(0.4), lineWidth: 0.75)

                if cruz {
                    Path { p in
                        let c = CGPoint(x: area.midX, y: area.midY)
                        p.move(to: CGPoint(x: c.x - 12, y: c.y)); p.addLine(to: CGPoint(x: c.x + 12, y: c.y))
                        p.move(to: CGPoint(x: c.x, y: c.y - 12)); p.addLine(to: CGPoint(x: c.x, y: c.y + 12))
                    }
                    .stroke(Color.white.opacity(0.75), lineWidth: 1)
                }

                if nivel { linhaDeNivel(area) }
            }
        }
        .allowsHitTesting(false)
    }

    /// Linha que gira conforme o aparelho. Amarela e parada em 0 quando está nivelado.
    private func linhaDeNivel(_ area: CGRect) -> some View {
        let meia = min(area.width * 0.2, 150)
        let vao: CGFloat = 26
        let cor: Color = nivelado ? .destaque : .white.opacity(0.8)
        let graus = nivelado ? 0 : (angulo ?? 0)
        return ZStack {
            Path { p in
                let y = area.midY
                p.move(to: CGPoint(x: area.midX - meia, y: y)); p.addLine(to: CGPoint(x: area.midX - vao, y: y))
                p.move(to: CGPoint(x: area.midX + vao, y: y)); p.addLine(to: CGPoint(x: area.midX + meia, y: y))
            }
            .stroke(cor, style: StrokeStyle(lineWidth: 2, lineCap: .round))
            .rotationEffect(.degrees(graus), anchor: UnitPoint(x: area.midX / max(area.maxX + area.minX, 1), y: area.midY / max(area.maxY + area.minY, 1)))
            .animation(reduzir ? nil : .easeOut(duration: 0.15), value: nivelado)
            // marcas fixas do horizonte de referência
            Path { p in
                let y = area.midY
                p.move(to: CGPoint(x: area.midX - meia - 14, y: y)); p.addLine(to: CGPoint(x: area.midX - meia - 4, y: y))
                p.move(to: CGPoint(x: area.midX + meia + 4, y: y)); p.addLine(to: CGPoint(x: area.midX + meia + 14, y: y))
            }
            .stroke(Color.white.opacity(0.35), lineWidth: 1)
        }
    }

    /// Retângulo do vídeo dentro da tela (resizeAspect).
    private func ajustar(_ t: CGSize) -> CGRect {
        var w = t.width, h = t.width / proporcao
        if h > t.height { h = t.height; w = h * proporcao }
        return CGRect(x: (t.width - w) / 2, y: (t.height - h) / 2, width: w, height: h)
    }
}

/// Só no simulador: um quadro de teste no lugar do preview, para o layout fazer sentido nos prints.
struct FundoDeTeste: View {
    var body: some View {
        GeometryReader { g in
            ZStack {
                LinearGradient(colors: [Color(red: 0.36, green: 0.52, blue: 0.70), Color(red: 0.85, green: 0.78, blue: 0.62)],
                               startPoint: .top, endPoint: .center)
                VStack(spacing: 0) {
                    Spacer()
                    Rectangle().fill(Color(red: 0.16, green: 0.20, blue: 0.17)).frame(height: g.size.height * 0.5)
                }
                HStack(spacing: 0) {
                    ForEach(0..<6, id: \.self) { i in
                        Rectangle().fill(Color.white.opacity(i % 2 == 0 ? 0.10 : 0.04))
                    }
                }
                .frame(height: g.size.height * 0.5)
                .offset(y: g.size.height * 0.25)
            }
        }
    }
}


// MARK: - Tela

struct TelaPrincipal: View {
    @ObservedObject var modelo: ModeloApp
    @Environment(\.accessibilityReduceMotion) private var reduzir

    private var anim: Animation? { reduzir ? nil : .easeInOut(duration: 0.2) }

    private var proporcao: CGFloat {
        guard let d = modelo.dimCamera, d.altura > 0 else { return 16.0 / 9.0 }
        return CGFloat(d.largura) / CGFloat(d.altura)
    }

    var body: some View {
        let el = modelo.elementos
        return ZStack {
            Color.black.ignoresSafeArea()
            #if targetEnvironment(simulator)
            GeometryReader { g in
                let a = MedidasTela.areaDoVideo(tela: g.size, proporcao: proporcao)
                FundoDeTeste().frame(width: a.width, height: a.height).position(x: a.midX, y: a.midY)
            }
            .ignoresSafeArea()
            #endif
            PreviewCamera(
                sessao: modelo.captura.sessao,
                aoTocar: { modelo.tocarNaImagem(camada: $0, dispositivo: $1, travar: $2) },
                aoToqueDuplo: { withAnimation(anim) { modelo.alternarLimpo() } },
                aoPinca: { modelo.pinca(escala: $0, fase: $1) })
                .ignoresSafeArea()
            GuiasView(grade: el.grade, cruz: el.cruz, nivel: el.nivel, angulo: modelo.anguloNivel,
                      nivelado: modelo.nivelado, proporcao: proporcao)
                .ignoresSafeArea()
            indicadorDeFoco.ignoresSafeArea()
            GeometryReader { g in
                camadaDeControles(MedidasTela.areaDoVideo(tela: g.size, proporcao: proporcao), el)
            }
            .ignoresSafeArea()
        }
        .animation(anim, value: el.controles)
        .animation(anim, value: modelo.painel)
        .animation(anim, value: modelo.grupo)
    }

    // MARK: composição (tudo em coordenadas da área do vídeo)

    private func camada<V: View>(_ alinhamento: Alignment, @ViewBuilder _ conteudo: () -> V) -> some View {
        conteudo().frame(maxWidth: .infinity, maxHeight: .infinity, alignment: alinhamento)
    }

    private func camadaDeControles(_ area: CGRect, _ el: ElementosVisiveis) -> some View {
        let lado = modelo.prefs.lado
        let borda = MedidasTela.borda
        let doMenu: Alignment = lado == .esquerda ? .topLeading : .topTrailing
        let dasLentes: Alignment = lado == .esquerda ? .trailing : .leading
        let limparCanto: Alignment = lado == .esquerda ? .bottomTrailing : .bottomLeading
        let recuos = MedidasTela.recuosDoPainel(lado: lado, trilhoAberto: modelo.grupo != nil)
        let posNoAr = PosicaoNoAr.de(noAr: modelo.noArTela, limpo: modelo.limpo)

        return ZStack(alignment: .topLeading) {
            if el.controles {
                camada(.top) { topoLinha.padding(.top, MedidasTela.topoY).padding(.horizontal, borda) }
                camada(doMenu) { colunas.padding(.top, MedidasTela.colunasY).padding(.bottom, borda).padding(.horizontal, borda) }
                camada(dasLentes) { lentes.padding(.horizontal, borda) }
                if let p = modelo.painel {
                    camada(.bottom) {
                        PainelInferior(modelo: modelo, painel: p)
                            .padding(.leading, recuos.esquerda).padding(.trailing, recuos.direita).padding(.bottom, borda)
                            .transition(.move(edge: .bottom).combined(with: .opacity))
                    }
                }
            }
            camada(limparCanto) { botaoLimpar.padding(borda) }
            if el.tally {
                Rectangle().strokeBorder(Color.red, lineWidth: 5).allowsHitTesting(false)
            }
            if posNoAr == .seloCentralizado {
                camada(.top) {
                    Text("NO AR").font(.system(size: 13, weight: .heavy)).foregroundColor(.white)
                        .padding(.horizontal, 10).padding(.vertical, 4).background(Capsule().fill(Color.red)).padding(.top, 14)
                }
                .allowsHitTesting(false)
                .accessibilityLabel("No ar")
            }
            if let t = modelo.aviso, el.controles {
                camada(.top) {
                    Text(t).font(.system(size: 12.5, weight: .semibold)).foregroundColor(.white)
                        .padding(.horizontal, 14).padding(.vertical, 7)
                        .background(Capsule().fill(Color.black.opacity(0.75)))
                        .padding(.top, 52)
                }
                .allowsHitTesting(false)
                .transition(.opacity)
            }
        }
        .frame(width: area.width, height: area.height, alignment: .topLeading)
        .position(x: area.midX, y: area.midY)
    }

    // MARK: indicador de foco

    @ViewBuilder private var indicadorDeFoco: some View {
        if let i = modelo.indicadorFoco {
            ZStack {
                RoundedRectangle(cornerRadius: 4).stroke(Color.destaque, lineWidth: 1.5).frame(width: 72, height: 72)
                if i.travado {
                    Text("AE/AF travado").font(.system(size: 11, weight: .bold)).foregroundColor(.black)
                        .padding(.horizontal, 6).padding(.vertical, 2).background(Capsule().fill(Color.destaque))
                        .offset(y: 52)
                }
            }
            .position(i.ponto)
            .allowsHitTesting(false)
        }
    }

    // MARK: topo

    private var topoLinha: some View {
        ViewThatFits(in: .horizontal) {
            linhaDoTopo(apertado: false)
            linhaDoTopo(apertado: true)
        }
    }

    /// A direita tem prioridade. Se nada couber, "Conectado" vira só a bolinha, bateria e temperatura perdem o
    /// ícone e o nome corta com reticências.
    private func linhaDoTopo(apertado: Bool) -> some View {
        let p = modelo.prefs
        let noArPilula = PosicaoNoAr.de(noAr: modelo.noArTela, limpo: modelo.limpo) == .pilulaNoTopo
        let temEsquerda = noArPilula || !p.itensEsquerda.isEmpty
        return HStack(spacing: 8) {
            if temEsquerda {
                capsulaEsquerda(noAr: noArPilula, apertado: apertado).layoutPriority(0)
            }
            Spacer(minLength: 0)
            if !p.itensDireita.isEmpty {
                capsulaDireita(apertado: apertado).fixedSize().layoutPriority(1)
            }
        }
    }

    private var estado: (texto: String, cor: Color) {
        switch modelo.info.estado {
        case .iniciando: return ("Iniciando", .gray)
        case .esperando: return ("Esperando", .white)
        case .conectado: return ("Conectado", Color(red: 0.19, green: 0.82, blue: 0.35))
        case .erroCamera, .erroRede: return ("Erro", .red)
        }
    }

    private func chipBase(_ fundo: Color = Color.white.opacity(0.08), _ cor: Color = .white, @ViewBuilder _ c: () -> some View) -> some View {
        HStack(spacing: 5) { c() }
            .font(.system(size: 12, weight: .semibold)).monospacedDigit()
            .foregroundColor(cor)
            .padding(.horizontal, 9).frame(height: 24)
            .background(Capsule().fill(fundo))
    }

    private func capsulaEsquerda(noAr: Bool, apertado: Bool) -> some View {
        let p = modelo.prefs
        return HStack(spacing: 6) {
            if noAr {
                Text("NO AR").font(.system(size: 12, weight: .heavy)).foregroundColor(.white)
                    .padding(.horizontal, 9).frame(height: 24).background(Capsule().fill(Color.red)).fixedSize()
                    .accessibilityLabel("No ar")
            }
            if p.mostra(.estado) {
                chipBase { Circle().fill(estado.cor).frame(width: 7, height: 7); if !apertado { Text(estado.texto) } }
                    .fixedSize()
                    .onTapGesture { modelo.mostrarAviso(modelo.detalheDoEstado) }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Estado: \(estado.texto). Toque para ver o detalhe")
                    .accessibilityAddTraits(.isButton)
            }
            if p.mostra(.nome) {
                Text(modelo.nome).font(.system(size: 13, weight: .heavy)).lineLimit(1).truncationMode(.tail)
                    .foregroundColor(.white).padding(.horizontal, 4)
            }
        }
        .padding(.horizontal, 10)
        .frame(height: MedidasTela.topoAltura)
        .vidro()
    }

    private func capsulaDireita(apertado: Bool) -> some View {
        let p = modelo.prefs
        let a = modelo.assinatura
        return HStack(spacing: 6) {
            ForEach(p.itensDireita, id: \.self) { item in
                switch item {
                case .formato:
                    chipBase { Text(Rotulos.formato(modelo.dimCamera)) }
                case .iso:
                    chipBase { Text(verbatim: "ISO \(modelo.leitura.iso)  \(modelo.leitura.obturadorTexto)") }
                case .bateria:
                    chipBase {
                        if !apertado { Image(systemName: modelo.carregando ? "battery.100.bolt" : "battery.75").font(.system(size: 12)) }
                        Text(modelo.bateria >= 0 ? "\(Int((modelo.bateria * 100).rounded()))%" : "-")
                    }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Bateria \(modelo.bateria >= 0 ? "\(Int((modelo.bateria * 100).rounded())) por cento" : "indisponível")")
                case .temperatura:
                    chipBase(Color.white.opacity(0.08), modelo.temperatura == "Normal" ? .white : .destaque) {
                        if !apertado { Image(systemName: "thermometer.medium").font(.system(size: 12)) }
                        Text(modelo.temperatura)
                    }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Temperatura \(modelo.temperatura)")
                case .validade:
                    if let a {
                        chipBase(a.vencida ? .red : (a.urgente ? .destaque : Color.white.opacity(0.08)),
                                 a.vencida ? .white : (a.urgente ? .black : .white)) {
                            Image(systemName: a.vencida ? "calendar.badge.exclamationmark" : "calendar").font(.system(size: 12))
                            Text(a.selo)
                        }
                        .accessibilityElement(children: .ignore)
                        .accessibilityLabel(a.texto)
                    }
                case .ip:
                    chipBase {
                        Image(systemName: "wifi").font(.system(size: 12))
                        Text(verbatim: modelo.copiado ? "Copiado" : (EnderecoRede.enderecoParaCopiar(ip: modelo.ipWifi, porta: Protocolo.portaPadrao) ?? "sem Wi-Fi"))
                    }
                    .onTapGesture { modelo.copiarEndereco() }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Endereço de rede. Toque para copiar")
                    .accessibilityAddTraits(.isButton)
                default:
                    EmptyView()
                }
            }
        }
        .padding(.leading, 8).padding(.trailing, 4)
        .frame(height: MedidasTela.topoAltura)
        .vidro()
    }

    // MARK: menu e trilho

    private var colunas: some View {
        let lado = modelo.prefs.lado
        return HStack(alignment: .top, spacing: 6) {
            if lado == .esquerda { menu; if modelo.grupo != nil { trilho } }
            else { if modelo.grupo != nil { trilho }; menu }
        }
    }

    private var menu: some View {
        let itens = VStack(spacing: 6) {
            ForEach(GrupoMenu.allCases, id: \.self) { g in
                ItemDoMenu(grupo: g, selecionado: modelo.grupo == g) { modelo.alternarGrupo(g) }
            }
        }
        return ViewThatFits(in: .vertical) {
            itens
            ScrollView(.vertical, showsIndicators: false) { itens }
        }
        .padding(.horizontal, 6).padding(.vertical, 8)
        .frame(width: MedidasTela.larguraMenu)
        .frame(maxHeight: .infinity)
        .vidro()
    }

    private var trilho: some View {
        VStack(spacing: 6) {
            Text(modelo.grupo?.titulo ?? "").font(.system(size: 8.5, weight: .bold)).tracking(0.8)
                .foregroundColor(.white.opacity(0.62)).padding(.top, 2)
            ScrollView(.vertical, showsIndicators: false) {
                VStack(spacing: 6) { botoesDoTrilho }
            }
        }
        .padding(.horizontal, 6).padding(.vertical, 8)
        .frame(width: MedidasTela.larguraTrilho)
        .frame(maxHeight: .infinity, alignment: .top)
        .vidro()
    }

    @ViewBuilder private var botoesDoTrilho: some View {
        let a = modelo.ajustes
        switch modelo.grupo {
        case .camera:
            BotaoTrilho(simbolo: "gyroscope", rotulo: "Estab", ativo: a.estabilizar,
                        descricao: a.estabilizar ? "Estabilização ligada. Toque para desligar" : "Estabilização desligada. Toque para ligar") {
                modelo.mudar { $0.estabilizar.toggle() }
            }
            BotaoTrilho(simbolo: a.exposicaoTravada ? "lock.fill" : "lock.open", rotulo: "Trava AE", ativo: a.exposicaoTravada,
                        descricao: "Trava de exposição") { modelo.alternarTrava(exposicao: true) }
            BotaoTrilho(simbolo: a.focoTravado ? "lock.fill" : "lock.open", rotulo: "Trava AF", ativo: a.focoTravado,
                        descricao: "Trava de foco") { modelo.alternarTrava(exposicao: false) }
            BotaoTrilho(simbolo: "thermometer.sun", rotulo: Rotulos.bb(a), ativo: Rotulos.bbAtivo(a), aberto: modelo.painel == .branco,
                        descricao: "Balanço de branco. Toque para abrir o ajuste") { modelo.alternarPainel(.branco) }
            BotaoTrilho(simbolo: "viewfinder", rotulo: Rotulos.foco(a), ativo: Rotulos.focoAtivo(a), aberto: modelo.painel == .foco,
                        descricao: "Foco. Toque para abrir o ajuste") { modelo.alternarPainel(.foco) }
            BotaoTrilho(simbolo: "plusminus.circle", rotulo: Rotulos.ev(a), ativo: Rotulos.evAtivo(a), aberto: modelo.painel == .exposicao,
                        descricao: "Exposição. Toque para abrir o ajuste") { modelo.alternarPainel(.exposicao) }
        case .guias:
            BotaoTrilho(simbolo: "grid", rotulo: "Grade", ativo: a.grade, descricao: "Grade de terços") { modelo.mudar { $0.grade.toggle() } }
            BotaoTrilho(simbolo: "plus", rotulo: "Cruz", ativo: a.centro, descricao: "Cruz central") { modelo.mudar { $0.centro.toggle() } }
            BotaoTrilho(simbolo: "level", rotulo: "Nível", ativo: a.nivel, descricao: "Nível horizontal") { modelo.mudar { $0.nivel.toggle() } }
        case .info:
            ForEach(ItemTopo.allCases, id: \.self) { i in
                BotaoTrilho(simbolo: i.simbolo, rotulo: i.rotuloCurto, ativo: modelo.prefs.mostra(i),
                            descricao: "\(i.nomeCompleto) na faixa de cima") { modelo.alternarItemDoTopo(i) }
            }
        case .ajustes:
            BotaoTrilho(simbolo: "tag", rotulo: "Nome", aberto: modelo.painel == .nome, descricao: "Nome da câmera") { modelo.alternarPainel(.nome) }
            BotaoTrilho(simbolo: "film", rotulo: "Qualidade", aberto: modelo.painel == .qualidade, descricao: "Qualidade de envio") { modelo.alternarPainel(.qualidade) }
            BotaoTrilho(simbolo: "wifi", rotulo: "Wi-Fi", aberto: modelo.painel == .wifi, descricao: "Endereço Wi-Fi") { modelo.alternarPainel(.wifi) }
            BotaoTrilho(simbolo: "calendar", rotulo: "Validade", aberto: modelo.painel == .validade, descricao: "Validade da assinatura") { modelo.alternarPainel(.validade) }
            BotaoTrilho(simbolo: "hand.tap", rotulo: "Gestos", aberto: modelo.painel == .gestos, descricao: "Gestos") { modelo.alternarPainel(.gestos) }
            BotaoTrilho(simbolo: "sidebar.left", rotulo: "Menu", aberto: modelo.painel == .lado, descricao: "Lado do menu") { modelo.alternarPainel(.lado) }
        case nil:
            EmptyView()
        }
    }

    // MARK: lentes e botão limpar

    private var lentes: some View {
        VStack(spacing: 0) {
            ForEach(modelo.lentes) { o in
                let ativa = modelo.estaSelecionada(o)
                let numero = o.rotulo.replacingOccurrences(of: "x", with: "")
                Button { modelo.selecionar(o) } label: {
                    Text(ativa ? "\(numero)x" : numero)
                        .font(.system(size: 13, weight: .bold, design: .rounded)).monospacedDigit()
                        .foregroundColor(ativa ? .black : .white)
                        .frame(width: 44, height: 44)
                        .background(Circle().fill(ativa ? Color.destaque : .clear))
                }
                .buttonStyle(EstiloPressionar())
                .accessibilityLabel("Lente \(numero.replacingOccurrences(of: ",", with: " vírgula ")) vezes")
                .accessibilityAddTraits(ativa ? .isSelected : [])
            }
        }
        .padding(4)
        .vidro(26)
        .opacity(modelo.lentes.isEmpty ? 0 : 1)
    }

    private var botaoLimpar: some View {
        Button {
            withAnimation(anim) { modelo.alternarLimpo() }
        } label: {
            Image(systemName: "eye.slash").font(.system(size: 18, weight: .semibold)).foregroundColor(.white)
                .frame(width: 56, height: 44)
                .vidro(14)
                .opacity(modelo.limpo ? 0.45 : 1)
        }
        .buttonStyle(EstiloPressionar())
        .accessibilityLabel(modelo.limpo ? "Voltar os controles" : "Limpar a tela. Esconde os controles e deixa só as guias")
    }
}

// MARK: - Painel inferior

struct PainelInferior: View {
    @ObservedObject var modelo: ModeloApp
    let painel: PainelRapido

    var body: some View {
        Group {
            switch painel {
            case .foco: foco
            case .exposicao: exposicao
            case .branco: branco
            case .nome: nome
            case .qualidade: qualidade
            case .wifi: wifi
            case .validade: validade
            case .gestos: texto("Toque foca. Toque longo trava foco e exposição. Toque duplo limpa a tela. Pinça muda o zoom.")
            case .lado: lado
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .vidro()
    }

    private func texto(_ t: String) -> some View {
        Text(t).font(.system(size: 12.5)).foregroundColor(.white).fixedSize(horizontal: false, vertical: true)
    }

    private func nota(_ t: String) -> some View {
        Text(t).font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).fixedSize(horizontal: false, vertical: true)
    }

    private var foco: some View {
        let a = modelo.ajustes
        return HStack(spacing: 12) {
            Segmento(opcoes: ["Auto", "Manual"], atual: a.focoModo == .manual ? "Manual" : "Auto") { v in
                modelo.mudar { m in
                    m.focoModo = v == "Manual" ? .manual : .auto
                    if v == "Manual" { m.posicaoLente = modelo.leitura.posicaoLente }
                }
            }
            if a.focoModo == .manual {
                Regua(rotulo: "Perto", valor: Binding(get: { modelo.ajustes.posicaoLente }, set: { v in modelo.mudar { $0.posicaoLente = v } }),
                      faixa: 0...1, texto: "\(Int(a.posicaoLente * 100))%", rotuloFim: "Longe")
            } else {
                nota("Foca sozinho e acompanha a cena. Toque na imagem para escolher o ponto.")
            }
        }
    }

    private var exposicao: some View {
        let a = modelo.ajustes
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                Segmento(opcoes: ["Auto", "Manual"], atual: a.exposicaoModo == .manual ? "Manual" : "Auto") { v in
                    modelo.mudar { m in
                        m.exposicaoModo = v == "Manual" ? .manual : .auto
                        if v == "Manual", modelo.leitura.iso > 0 {
                            m.iso = Double(modelo.leitura.iso)
                            m.obturador = modelo.leitura.obturadorSegundos
                        }
                    }
                }
                if a.exposicaoModo == .auto {
                    Regua(rotulo: "EV", valor: Binding(get: { modelo.ajustes.bias }, set: { v in modelo.mudar { $0.bias = v } }),
                          faixa: -2...2, passo: 0.1, texto: String(format: "%+.1f EV", a.bias))
                } else {
                    Regua(rotulo: "ISO", valor: Binding(get: { modelo.ajustes.iso }, set: { v in modelo.mudar { $0.iso = v } }),
                          faixa: modelo.limites.isoMin...max(modelo.limites.isoMax, modelo.limites.isoMin + 1), texto: "\(Int(a.iso))")
                }
            }
            if a.exposicaoModo == .manual {
                Regua(rotulo: "Obt.", valor: Binding(get: { log(modelo.ajustes.obturador) }, set: { v in modelo.mudar { $0.obturador = exp(v) } }),
                      faixa: log(max(modelo.limites.obturadorMin, 1.0 / 32000))...log(max(modelo.limites.obturadorMax, modelo.limites.obturadorMin * 2)),
                      texto: Obturador.texto(a.obturador))
            }
        }
    }

    private var branco: some View {
        let a = modelo.ajustes
        return HStack(spacing: 12) {
            Segmento(opcoes: ["Auto", "Travado"], atual: a.brancoTravado ? "Travado" : "Auto") { v in
                modelo.mudar { $0.brancoTravado = v == "Travado"; if v != "Travado" { $0.kelvin = nil } }
            }
            if a.brancoTravado {
                Regua(rotulo: "Temperatura", valor: Binding(get: { modelo.ajustes.kelvin ?? 5500 }, set: { v in modelo.mudar { $0.kelvin = v } }),
                      faixa: 2500...8000, passo: 100, texto: "\(Int(a.kelvin ?? 5500)) K")
            } else {
                nota("O iPhone ajusta o branco sozinho.")
            }
        }
    }

    private var nome: some View {
        HStack(spacing: 10) {
            Text("Nome desta câmera (aparece no OBS)").font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).fixedSize()
            TextField("Ex.: Palco 1", text: $modelo.campoNome)
                .font(.system(size: 13)).foregroundColor(.white).submitLabel(.done)
                .onSubmit { modelo.salvarNome() }
                .padding(.horizontal, 9).frame(minHeight: 36)
                .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.white.opacity(0.1)))
        }
    }

    private var qualidade: some View {
        let formatoTrav = modelo.formatoTravado
        let bitrateTrav = modelo.bitrateTravado
        // Travado: mostra o que está saindo de verdade (veio do OBS). Livre: o que está salvo no iPhone.
        let preset = formatoTrav ? QualidadeEnvio.preset(de: modelo.dimCamera) : modelo.ajustes.preset
        let bitrate = bitrateTrav ? modelo.info.bitrate : modelo.ajustes.bitrate
        let sup = modelo.presetsSuportados
        let resolucoes = Resolucao.allCases.map(\.rawValue)
        let semResolucao: Set<String> = formatoTrav ? Set(resolucoes) : Set(Resolucao.allCases.filter { r in
            !QualidadeEnvio.quadros.contains { QualidadeEnvio.disponivel(r, $0, suportados: sup) }
        }.map(\.rawValue))
        let resAtual = preset.map { Resolucao.de($0) }
        let quadros = QualidadeEnvio.quadros.map { String($0) }
        let semQuadros: Set<String> = formatoTrav ? Set(quadros) : Set(QualidadeEnvio.quadros.filter { f in
            guard let r = resAtual else { return true }
            return !QualidadeEnvio.disponivel(r, f, suportados: sup)
        }.map { String($0) })
        let bitrates = QualidadeEnvio.bitrates.map { QualidadeEnvio.numero(bitrate: $0) }
        return VStack(alignment: .leading, spacing: 6) {
            linhaDeQualidade("Resolução") {
                Segmento(opcoes: resolucoes, atual: resAtual?.rawValue ?? "", desabilitados: semResolucao, compacto: true) { v in
                    guard let r = Resolucao(rawValue: v), let novo = QualidadeEnvio.trocar(resolucao: r, fpsAtual: modelo.ajustes.preset.fps, suportados: sup) else { return }
                    modelo.mudar { $0.preset = novo }
                }
            }
            linhaDeQualidade("Quadros") {
                Segmento(opcoes: quadros, atual: preset.map { String($0.fps) } ?? "", desabilitados: semQuadros, compacto: true) { v in
                    guard let f = Int(v), let novo = QualidadeEnvio.preset(resolucao: Resolucao.de(modelo.ajustes.preset), fps: f) else { return }
                    modelo.mudar { $0.preset = novo }
                }
            }
            linhaDeQualidade("Bitrate") {
                HStack(spacing: 8) {
                    Segmento(opcoes: bitrates, atual: QualidadeEnvio.numero(bitrate: bitrate), desabilitados: bitrateTrav ? Set(bitrates) : [], compacto: true) { v in
                        if let i = bitrates.firstIndex(of: v), i < QualidadeEnvio.bitrates.count {
                            let b = QualidadeEnvio.bitrates[i]
                            modelo.mudar { $0.bitrate = b }
                        }
                    }
                    Text("Mbps").font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).fixedSize()
                }
            }
            if let t = QualidadeEnvio.aviso(presetObs: formatoTrav, bitrateObs: bitrateTrav) { nota(t) }
        }
    }

    private func linhaDeQualidade<C: View>(_ rotulo: String, @ViewBuilder _ conteudo: () -> C) -> some View {
        HStack(spacing: 8) {
            Text(rotulo).font(.system(size: 12)).foregroundColor(.white.opacity(0.62)).lineLimit(1).minimumScaleFactor(0.8)
                .frame(width: 58, alignment: .leading)
            conteudo()
        }
    }

    private var wifi: some View {
        VStack(alignment: .leading, spacing: 3) {
            if let ip = modelo.ipWifi {
                Text(verbatim: "Endereço \(ip), porta \(Protocolo.portaPadrao).").font(.system(size: 12.5, weight: .semibold)).foregroundColor(.white)
                nota("No OBS o iPhone aparece sozinho. Se não aparecer, digite esse endereço.")
            } else {
                Text("Wi-Fi desligado.").font(.system(size: 12.5, weight: .semibold)).foregroundColor(.white)
                nota("Use o cabo, ou ligue o Wi-Fi para conectar pela rede.")
            }
        }
    }

    private var validade: some View {
        VStack(alignment: .leading, spacing: 3) {
            if let a = modelo.assinatura {
                Text(a.vencida ? "Assinatura vencida em \(a.quando)." : "Assinatura vence em \(a.quando).")
                    .font(.system(size: 12.5, weight: .semibold)).foregroundColor(a.vencida ? .red : (a.urgente ? .destaque : .white))
                nota("Renova pelo SideStore.")
            } else {
                Text("Este aparelho não tem perfil de assinatura.").font(.system(size: 12.5)).foregroundColor(.white)
            }
        }
    }

    private var lado: some View {
        HStack(spacing: 12) {
            Segmento(opcoes: LadoMenu.allCases.map(\.rotulo), atual: modelo.prefs.lado.rotulo) { v in
                modelo.definirLado(v == "Direita" ? .direita : .esquerda)
            }
            nota("Lado do menu e do trilho. As lentes vão para o outro lado.")
        }
    }
}
