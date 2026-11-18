import Foundation
import Network
import os

enum Log {
    static let subsistema = "com.fmrocket.fmcam"
    static let servidor = Logger(subsystem: subsistema, category: "servidor")
    static let captura = Logger(subsystem: subsistema, category: "captura")
    static let codificador = Logger(subsystem: subsistema, category: "codificador")
    static let app = Logger(subsystem: subsistema, category: "app")
}

struct IdentidadeAparelho: Equatable {
    var modelo: String
    var nome: String
    var id: String
}

enum EstadoServidor: Equatable {
    case iniciando
    case esperando
    case conectado(String)
    case erroCamera(String)
    case erroRede(String)
}

struct InfoServidor: Equatable {
    var estado: EstadoServidor = .iniciando
    var fps: Double = 0
    var kbps: Int = 0
    var resolucao: String?
    var origem: OrigemConexao?
    /// Tally: a extensão avisou que esta fonte está no Programa do OBS.
    var noAr = false
    /// O OBS fixou o preset (Resolução e Quadros ficam só leitura) e/ou o bitrate nesta conexão.
    var presetObs = false
    var bitrateObs = false
    var qualidadeObs: Bool { presetObs || bitrateObs }
    /// Bitrate em uso (bps), 0 sem conexão.
    var bitrate = 0
}

/// Lê bateria e carregamento (alimentado pela tela, que roda na thread principal).
final class CaixaSensores {
    private let trava = NSLock()
    private var _bateria = -1.0
    private var _carregando = false

    func atualizar(bateria: Double, carregando: Bool) {
        trava.lock(); _bateria = bateria; _carregando = carregando; trava.unlock()
    }
    func ler() -> (bateria: Double, carregando: Bool) {
        trava.lock(); defer { trava.unlock() }
        return (_bateria, _carregando)
    }
}

/// Campos de câmera do status tipo 3 (nomes e valores fixados em docs/PROTOCOLO.md).
enum StatusCamera {
    static func campos(ajustes a: AjustesCamera, leitura: LeituraCamera, noAr: Bool, dim: DimensoesVideo?,
                       assinaturaVence: Date? = nil) -> [String: Any] {
        let foco = a.focoModo == .manual ? "manual" : (a.focoTravado ? "travado" : a.focoModo.rawValue)
        let exposicao = a.exposicaoModo == .manual ? "manual" : (a.exposicaoTravada ? "travada" : "auto")
        var o: [String: Any] = [
            "lente": a.lente.rawValue,
            "zoom": (a.zoom * 100).rounded() / 100,
            "foco": foco,
            "exposicao": exposicao,
            "branco": a.brancoTravado ? "travado" : "auto",
            "bias": (a.bias * 100).rounded() / 100,
            "iso": leitura.iso,
            "obturador": leitura.obturadorTexto,
            "noar": noAr,
        ]
        if let v = assinaturaVence { o["assinatura_vence"] = Assinatura.iso(v) }
        if let d = dim { o["w"] = d.largura; o["h"] = d.altura; o["fps_alvo"] = d.fps }
        return o
    }
}

func temperaturaTexto(_ e: ProcessInfo.ThermalState) -> String {
    switch e {
    case .nominal: return "nominal"
    case .fair: return "fair"
    case .serious: return "serious"
    case .critical: return "critical"
    @unknown default: return "nominal"
    }
}

/// Nome da câmera dado pelo dono (UserDefaults). Padrão "iPhone " + 4 primeiros caracteres do id, para nunca repetir.
enum NomeCamera {
    static let chave = "nomeCamera"
    static let tamanhoMaximo = 40

    static func padrao(idVendor: String) -> String {
        let sufixo = String(idVendor.prefix(4)).uppercased()
        return sufixo.isEmpty ? "iPhone" : "iPhone \(sufixo)"
    }

    static func ler(defaults: UserDefaults = .standard, idVendor: String) -> String {
        if let salvo = defaults.string(forKey: chave) {
            let limpo = salvo.trimmingCharacters(in: .whitespacesAndNewlines)
            if !limpo.isEmpty { return limpo }
        }
        return padrao(idVendor: idVendor)
    }

    /// Nome vazio volta ao padrão. Devolve o nome efetivo.
    @discardableResult
    static func salvar(_ nome: String, defaults: UserDefaults = .standard, idVendor: String) -> String {
        let limpo = String(nome.trimmingCharacters(in: .whitespacesAndNewlines).prefix(tamanhoMaximo))
        if limpo.isEmpty { defaults.removeObject(forKey: chave) } else { defaults.set(limpo, forKey: chave) }
        return ler(defaults: defaults, idVendor: idVendor)
    }
}

final class Servidor {
    struct Opcoes {
        var porta: UInt16 = Protocolo.portaPadrao
        var timeoutHandshake: TimeInterval = 3
        var timeoutInatividade: TimeInterval = 5
        var intervaloStatus: TimeInterval = 2
        /// Acima disso, quadros que não são chave são descartados até o próximo keyframe.
        /// Piso do limite da fila de envio. O limite real é o maior entre este e 1 s de vídeo no bitrate pedido.
        var limiteFilaBytes = 1024 * 1024
        /// Se a câmera não ficar pronta nesse tempo, responde erro "camera".
        var timeoutPartidaCamera: TimeInterval = 2.5
        /// Sem quadros por esse tempo durante a transmissão, a sessão é encerrada (cliente reconecta).
        var timeoutSemQuadros: TimeInterval = 1.5
        /// Folga para o primeiro quadro chegar depois que a câmera ficou pronta.
        var folgaPrimeiroQuadro: TimeInterval = 5
        var intervaloLog: TimeInterval = 10
        var bonjour = true
        /// Teto de memória da fila de envio: acima disso a conexão é fechada.
        var tetoFilaBytes = 8 * 1024 * 1024
        /// Pedido de quadro-chave por fila cheia: no máximo um por esse intervalo.
        var intervaloMinimoKeyframe: TimeInterval = 1
    }

    var aoAtualizar: ((InfoServidor) -> Void)?

    private let opcoes: Opcoes
    private var identidade: IdentidadeAparelho
    private let criarFonte: () -> FonteDeVideo
    private let sensores: CaixaSensores
    private let ajustes: CaixaAjustes
    private let fila = DispatchQueue(label: "fmcam.servidor")
    private var listener: NWListener?
    private var sessao: Sessao?
    private var info = InfoServidor()
    private var parado = false
    private let travaPorta = NSLock()
    private var _porta: UInt16?
    private var _picoFila = 0

    /// Maior tamanho que a fila de envio chegou a ter (diagnóstico e testes).
    var picoBytesNaFila: Int {
        travaPorta.lock(); defer { travaPorta.unlock() }
        return _picoFila
    }

    init(opcoes: Opcoes = Opcoes(), identidade: IdentidadeAparelho, sensores: CaixaSensores = CaixaSensores(),
         ajustes: CaixaAjustes = CaixaAjustes(), criarFonte: @escaping () -> FonteDeVideo) {
        self.ajustes = ajustes
        self.opcoes = opcoes
        self.identidade = identidade
        self.sensores = sensores
        self.criarFonte = criarFonte
    }

    var portaAtual: UInt16? {
        travaPorta.lock(); defer { travaPorta.unlock() }
        return _porta
    }

    func iniciar() { fila.async { self.abrirListener() } }

    /// Vale a partir da próxima conexão.
    func atualizarNome(_ nome: String) {
        fila.async {
            self.identidade.nome = nome
            if self.opcoes.bonjour { self.listener?.service = Bonjour.servico(nome: nome, id: self.identidade.id) }
        }
    }

    /// A qualidade salva no iPhone mudou. Se tem conexão e o OBS não fixou, reconfigura sem derrubar.
    func qualidadeMudou() { fila.async { self.sessao?.qualidadeSalvaMudou() } }

    func parar() {
        fila.async {
            self.parado = true
            self.listener?.cancel()
            self.listener = nil
            self.sessao?.encerrar("servidor parado")
        }
    }

    private func estaSemEstadoDefinido() -> Bool {
        switch info.estado {
        case .iniciando, .erroRede: return true
        default: return false
        }
    }

    private func publicar() { aoAtualizar?(info) }

    private func abrirListener() {
        guard !parado else { return }
        do {
            let params = NWParameters.tcp
            params.allowLocalEndpointReuse = true
            if let tcp = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options { tcp.noDelay = true }
            let porta = NWEndpoint.Port(rawValue: opcoes.porta) ?? .any
            let l = try NWListener(using: params, on: porta)
            if opcoes.bonjour { l.service = Bonjour.servico(nome: identidade.nome, id: identidade.id) }
            l.stateUpdateHandler = { [weak self, weak l] estado in
                guard let self, let l else { return }
                switch estado {
                case .ready:
                    self.travaPorta.lock(); self._porta = l.port?.rawValue; self.travaPorta.unlock()
                    if self.sessao == nil, self.estaSemEstadoDefinido() {
                        self.info.estado = .esperando
                        self.publicar()
                    }
                case .failed(let erro):
                    l.cancel()
                    self.info.estado = .erroRede("Rede indisponível (\(erro.debugDescription))")
                    self.publicar()
                    self.fila.asyncAfter(deadline: .now() + 2) { self.abrirListener() }
                default: break
                }
            }
            l.newConnectionHandler = { [weak self] c in self?.novaConexao(c) }
            listener = l
            l.start(queue: fila)
        } catch {
            info.estado = .erroRede("Não foi possível abrir a porta \(opcoes.porta).")
            publicar()
            fila.asyncAfter(deadline: .now() + 2) { self.abrirListener() }
        }
    }

    private func novaConexao(_ c: NWConnection) {
        if sessao != nil {
            // Lê a linha do cliente (até timeoutHandshake) antes de responder: fechar sem ler gera RST e o Windows perde a resposta.
            Rejeicao(conexao: c, fila: fila, timeout: opcoes.timeoutHandshake).iniciar()
            return
        }
        let s = Sessao(conexao: c, fila: fila, opcoes: opcoes, identidade: identidade,
                       sensores: sensores, ajustes: ajustes, fonte: criarFonte())
        sessao = s
        info.origem = EnderecoRede.origem(de: c.endpoint)
        s.aoMudar = { [weak self] estado, resolucao in
            guard let self else { return }
            self.info.estado = estado
            if let resolucao { self.info.resolucao = resolucao }
            self.publicar()
        }
        s.aoQualidade = { [weak self] q in
            guard let self else { return }
            self.info.presetObs = q.presetObs
            self.info.bitrateObs = q.bitrateObs
            self.info.bitrate = q.bitrate
            self.publicar()
        }
        s.aoNoAr = { [weak self] v in
            guard let self else { return }
            self.ajustes.noAr = v
            self.info.noAr = v
            self.publicar()
        }
        s.aoFila = { [weak self] total in
            guard let self else { return }
            self.travaPorta.lock(); self._picoFila = max(self._picoFila, total); self.travaPorta.unlock()
        }
        s.aoMetricas = { [weak self] fps, kbps in
            guard let self else { return }
            self.info.fps = fps
            self.info.kbps = kbps
            self.publicar()
        }
        s.aoEncerrar = { [weak self] erroCamera in
            guard let self else { return }
            self.sessao = nil
            self.info.fps = 0
            self.info.kbps = 0
            self.info.resolucao = nil
            self.info.origem = nil
            self.info.noAr = false
            self.info.presetObs = false
            self.info.bitrateObs = false
            self.info.bitrate = 0
            self.ajustes.noAr = false
            self.info.estado = erroCamera.map { .erroCamera($0) } ?? .esperando
            self.publicar()
        }
        s.iniciar()
    }
}

// MARK: - Sessão (um cliente)

private struct Saida {
    var dados: Data
    var tipo: UInt8
}

private final class Sessao {
    enum Fase { case handshake, iniciando, transmitindo, encerrando, encerrada }

    let conexao: NWConnection
    let fila: DispatchQueue
    let opcoes: Servidor.Opcoes
    let identidade: IdentidadeAparelho
    let sensores: CaixaSensores
    let ajustes: CaixaAjustes
    let fonte: FonteDeVideo

    var aoMudar: ((EstadoServidor, String?) -> Void)?
    var aoMetricas: ((Double, Int) -> Void)?
    var aoFila: ((Int) -> Void)?
    var aoNoAr: ((Bool) -> Void)?
    var aoQualidade: ((QualidadeEnvio) -> Void)?
    var aoEncerrar: ((String?) -> Void)?

    private var fase = Fase.handshake
    private var bufferHandshake = Data()
    private var pendenteApos = Data()
    private var eventosPendentes: [EventoVideo] = []
    private var parser = ParserPacotes()
    private var saida: [Saida] = []
    private var bytesSaida = 0
    private var emVoo = 0
    private var bytesNaFila: Int { bytesSaida + emVoo }
    private var limiteFila = 1024 * 1024
    private var ultimoPedidoKeyframe = Date.distantPast
    /// Quanto já entregue ao sistema de rede e ainda não escoado. Acima disso o resto espera na nossa fila.
    private static let janelaEnvio = 256 * 1024
    private static let limitePendenteIniciando = 64 * 1024
    private var aguardandoChave = true
    private var ultimoRecebido = Date()
    private var erroCamera: String?
    private var quadrosJanela = 0
    private var bytesJanela = 0
    private var inicioJanela = Date()
    private var timers: [DispatchSourceTimer] = []
    private var inicioPartida = Date()
    private var ultimoQuadro = Date()
    private var recebeuQuadro = false
    private var quadrosLog = 0
    private var descartadosLog = 0
    private var ultimoAvisoFila = Date.distantPast
    private static let contador = OSAllocatedUnfairLock(initialState: 0)
    let numero: Int = Sessao.contador.withLock { $0 += 1; return $0 }

    init(conexao: NWConnection, fila: DispatchQueue, opcoes: Servidor.Opcoes, identidade: IdentidadeAparelho,
         sensores: CaixaSensores, ajustes: CaixaAjustes, fonte: FonteDeVideo) {
        self.ajustes = ajustes
        self.conexao = conexao
        self.fila = fila
        self.opcoes = opcoes
        self.identidade = identidade
        self.sensores = sensores
        self.fonte = fonte
    }

    func iniciar() {
        conexao.stateUpdateHandler = { [weak self] estado in
            switch estado {
            case .failed(let e): self?.encerrar("conexão falhou: \(e.debugDescription)")
            case .cancelled: self?.encerrar("conexão cancelada")
            default: break
            }
        }
        Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): nova conexão")
        conexao.start(queue: fila)
        receber()
        criarTimer(depois: opcoes.timeoutHandshake, repete: nil) { [weak self] in
            guard let self, self.fase == .handshake else { return }
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): handshake não chegou a tempo")
            self.fecharComErro("handshake")
        }
    }

    private func criarTimer(depois: TimeInterval, repete: TimeInterval?, _ bloco: @escaping () -> Void) {
        let t = DispatchSource.makeTimerSource(queue: fila)
        t.schedule(deadline: .now() + depois, repeating: repete ?? .infinity)
        t.setEventHandler(handler: bloco)
        t.resume()
        timers.append(t)
    }

    private func receber() {
        conexao.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] dados, _, fim, erro in
            guard let self, self.fase != .encerrada else { return }
            if let d = dados, !d.isEmpty { self.tratar(d) }
            if fim || erro != nil { self.encerrar(fim ? "cliente fechou" : "erro de leitura: \(erro.debugDescription)"); return }
            if self.fase != .encerrada { self.receber() }
        }
    }

    private func tratar(_ d: Data) {
        ultimoRecebido = Date()
        switch fase {
        case .handshake:
            bufferHandshake.append(d)
            if let idx = bufferHandshake.firstIndex(of: 0x0A) {
                let linha = Data(bufferHandshake[bufferHandshake.startIndex..<idx])
                let resto = Data(bufferHandshake[(idx + 1)...])
                bufferHandshake = Data()
                tratarLinha(linha, resto: resto)
            } else if bufferHandshake.count > Protocolo.limiteHandshake {
                fecharComErro("handshake")
            }
        case .iniciando:
            pendenteApos.append(d)
            if pendenteApos.count > Sessao.limitePendenteIniciando {
                encerrar("mais de \(Sessao.limitePendenteIniciando) bytes do cliente durante a partida da câmera")
            }
        case .transmitindo:
            processarPacotes(d)
        default: break
        }
    }

    private func tratarLinha(_ linha: Data, resto: Data) {
        switch Handshake.interpretar(linha: linha) {
        case .invalido:
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): handshake inválido")
            fecharComErro("handshake")
        case .versaoNaoSuportada(let v):
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): versão \(v, privacy: .public) não suportada")
            fecharComErro("versao")
        case .aceito(let pedido):
            fase = .iniciando
            pendenteApos = resto
            ultimoRecebido = Date()
            qualidade = QualidadeEnvio.resolver(pedido: pedido, salva: ajustes.valor)
            limiteFila = max(opcoes.limiteFilaBytes, qualidade.bitrate / 8)
            if resto.count > Sessao.limitePendenteIniciando {
                encerrar("mais de \(Sessao.limitePendenteIniciando) bytes do cliente durante a partida da câmera")
                return
            }
            // Cliente sem dados por timeoutInatividade cai também na partida (nada de cliente fantasma segurando a câmera).
            let passoInatividade = max(0.05, min(0.5, opcoes.timeoutInatividade / 4))
            criarTimer(depois: passoInatividade, repete: passoInatividade) { [weak self] in
                guard let self, self.fase == .iniciando || self.fase == .transmitindo else { return }
                if Date().timeIntervalSince(self.ultimoRecebido) > self.opcoes.timeoutInatividade {
                    self.encerrar("cliente sem dados por \(self.opcoes.timeoutInatividade) s")
                }
            }
            let nome = pedido.cliente.isEmpty ? "cliente" : pedido.cliente
            clienteNome = nome
            inicioPartida = Date()
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): handshake recebido de \(nome, privacy: .public), preset \(self.qualidade.preset.rawValue, privacy: .public), lente \(pedido.lente.rawValue, privacy: .public), bitrate \(self.qualidade.bitrate, privacy: .public), escolhida no OBS \(self.qualidade.escolhidaNoOBS, privacy: .public). Iniciando a câmera")
            criarTimer(depois: opcoes.timeoutPartidaCamera, repete: nil) { [weak self] in
                guard let self, self.fase == .iniciando else { return }
                Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): câmera não ficou pronta em \(self.opcoes.timeoutPartidaCamera, privacy: .public) s")
                self.erroCamera = "A câmera demorou para iniciar."
                self.fecharComErro("camera")
            }
            fonte.aoEvento = { [weak self] ev in self?.fila.async { self?.tratarEvento(ev) } }
            let config = ConfigVideo(preset: qualidade.preset, lente: pedido.lenteInformada ? pedido.lente : nil, bitrate: qualidade.bitrate)
            fonte.iniciar(config) { [weak self] r in
                self?.fila.async { self?.aoIniciarFonte(r) }
            }
        }
    }

    private var clienteNome = ""
    private var dimAtual: DimensoesVideo?
    /// Qualidade em uso na conexão: do OBS (se ele fixou) ou a salva no iPhone.
    private var qualidade = QualidadeEnvio(preset: .p1080_30, bitrate: PedidoHandshake.bitratePadrao, presetObs: false, bitrateObs: false)

    private func aoIniciarFonte(_ r: Result<DimensoesVideo, ErroFonte>) {
        guard fase == .iniciando else { return }
        switch r {
        case .failure(let e):
            erroCamera = e.mensagem
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): câmera falhou: \(e.mensagem, privacy: .public)")
            fecharComErro("camera")
        case .success(let dim):
            let resposta = Handshake.respostaOk(aparelho: identidade.modelo, nome: identidade.nome, id: identidade.id,
                                                largura: dim.largura, altura: dim.altura, fps: dim.fps)
            conexao.send(content: resposta, completion: .contentProcessed { _ in })
            fase = .transmitindo
            dimAtual = dim
            inicioJanela = Date()
            ultimoRecebido = Date()
            ultimoQuadro = Date()
            let ms = Int(Date().timeIntervalSince(inicioPartida) * 1000)
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): câmera pronta em \(ms, privacy: .public) ms, \(dim.largura, privacy: .public)x\(dim.altura, privacy: .public) a \(dim.fps, privacy: .public) fps. Transmitindo")
            criarTimer(depois: opcoes.intervaloLog, repete: opcoes.intervaloLog) { [weak self] in
                guard let self, self.fase == .transmitindo else { return }
                let fps = Double(self.quadrosLog) / self.opcoes.intervaloLog
                Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): fps \(fps, format: .fixed(precision: 1), privacy: .public), bytesNaFila \(self.bytesNaFila, privacy: .public), descartados \(self.descartadosLog, privacy: .public)")
                self.quadrosLog = 0
                self.descartadosLog = 0
            }
            let passoWd = max(0.05, min(0.25, opcoes.timeoutSemQuadros / 4))
            criarTimer(depois: passoWd, repete: passoWd) { [weak self] in
                guard let self, self.fase == .transmitindo else { return }
                let limite = self.recebeuQuadro ? self.opcoes.timeoutSemQuadros : self.opcoes.folgaPrimeiroQuadro
                let parado = Date().timeIntervalSince(self.ultimoQuadro)
                if parado > limite {
                    Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): fluxo de quadros parou há \(parado, format: .fixed(precision: 1), privacy: .public) s (watchdog)")
                    self.encerrar("watchdog: sem quadros")
                }
            }
            aoMudar?(.conectado(clienteNome), "\(dim.largura)x\(dim.altura) a \(dim.fps) fps")
            aoQualidade?(qualidade)
            criarTimer(depois: opcoes.intervaloStatus, repete: opcoes.intervaloStatus) { [weak self] in self?.enviarStatus() }
            let eventos = eventosPendentes
            eventosPendentes = []
            eventos.forEach(tratarEvento)
            let resto = pendenteApos
            pendenteApos = Data()
            if !resto.isEmpty { processarPacotes(resto) }
        }
    }

    private func tratarEvento(_ ev: EventoVideo) {
        if fase == .iniciando {
            if eventosPendentes.count < 64 { eventosPendentes.append(ev) }
            return
        }
        guard fase == .transmitindo else { return }
        switch ev {
        case .config(let tempo, let dados):
            // O quadro seguinte a um tipo 1 é sempre chave.
            aguardandoChave = true
            enviar(Pacote(tipo: TipoPacote.config, tempo: tempo, dados: dados))
        case .quadro(let tempo, let chave, let dados):
            ultimoQuadro = Date()
            recebeuQuadro = true
            quadrosLog += 1
            let tamanho = dados.count + Protocolo.tamanhoCabecalho + 1
            if bytesNaFila > limiteFila / 2, Date().timeIntervalSince(ultimoAvisoFila) > 1 {
                ultimoAvisoFila = Date()
                Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): bytesNaFila alto: \(self.bytesNaFila, privacy: .public) de \(self.limiteFila, privacy: .public)")
            }
            if chave {
                // O quadro-chave mais novo sempre é enviado inteiro. Se não couber, o que está na fila antes dele é descartado.
                if bytesNaFila + tamanho > limiteFila {
                    let n = descartarQuadrosDaSaida()
                    descartadosLog += n
                    Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): fila cheia, \(n, privacy: .public) quadros velhos descartados antes do quadro-chave")
                }
                aguardandoChave = false
            } else {
                if aguardandoChave { descartadosLog += 1; return }
                if bytesNaFila + tamanho > limiteFila {
                    // Cliente lento: não acumula atraso, descarta até o próximo quadro-chave.
                    aguardandoChave = true
                    descartadosLog += 1
                    Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): fila cheia (\(self.bytesNaFila, privacy: .public) bytes), descartando até o próximo quadro-chave")
                    if Date().timeIntervalSince(ultimoPedidoKeyframe) >= opcoes.intervaloMinimoKeyframe {
                        ultimoPedidoKeyframe = Date()
                        fonte.pedirKeyframe()
                    }
                    return
                }
            }
            quadrosJanela += 1
            bytesJanela += dados.count
            enviar(Pacote.quadro(tempo: tempo, chave: chave, annexB: dados))
            if bytesNaFila > opcoes.tetoFilaBytes {
                encerrar("fila de envio acima do teto de \(opcoes.tetoFilaBytes) bytes")
            }
        case .erro(let msg):
            Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): erro da fonte: \(msg, privacy: .public)")
            erroCamera = msg
            encerrar("erro da câmera: \(msg)")
        }
    }

    private func enviar(_ p: Pacote) {
        let d = p.serializar()
        saida.append(Saida(dados: d, tipo: p.tipo))
        bytesSaida += d.count
        aoFila?(bytesNaFila)
        bombear()
    }

    /// Passa para o sistema de rede só o que cabe na janela. O resto fica aqui, onde ainda dá para descartar.
    private func bombear() {
        while let proximo = saida.first, fase == .transmitindo,
              emVoo == 0 || emVoo + proximo.dados.count <= Sessao.janelaEnvio {
            saida.removeFirst()
            bytesSaida -= proximo.dados.count
            emVoo += proximo.dados.count
            let n = proximo.dados.count
            conexao.send(content: proximo.dados, completion: .contentProcessed { [weak self] _ in
                self?.fila.async {
                    guard let self else { return }
                    self.emVoo -= n
                    self.bombear()
                }
            })
        }
    }

    /// Tira da fila os quadros (tipo 2). Config, status e pong ficam. Devolve quantos saíram.
    private func descartarQuadrosDaSaida() -> Int {
        let antes = saida.count
        saida.removeAll { $0.tipo == TipoPacote.quadro }
        bytesSaida = saida.reduce(0) { $0 + $1.dados.count }
        return antes - saida.count
    }

    private func processarPacotes(_ d: Data) {
        do {
            for p in try parser.alimentar(d) {
                switch p.tipo {
                case TipoPacote.ping:
                    enviar(Pacote.pong(tempo: relogioHostMicros(), eco: p.tempo))
                case TipoPacote.comando:
                    guard case .objeto(let membros)? = JSONEstrito.interpretar(p.dados),
                          case .texto(let cmd)? = membros.first(where: { $0.0 == "cmd" })?.1 else { break }
                    if cmd == "keyframe" {
                        fonte.pedirKeyframe()
                    } else if cmd == "noar", case .booleano(let v)? = membros.first(where: { $0.0 == "valor" })?.1 {
                        aoNoAr?(v)
                    }
                default: break
                }
            }
        } catch {
            encerrar("erro de protocolo: \(error)")
        }
    }

    /// Mudança de qualidade no iPhone durante a conexão. Não vale se o OBS fixou preset ou bitrate.
    func qualidadeSalvaMudou() {
        guard fase == .transmitindo, !(qualidade.presetObs && qualidade.bitrateObs) else { return }
        let nova = qualidade.comSalva(ajustes.valor)
        guard nova.preset != qualidade.preset || nova.bitrate != qualidade.bitrate else { return }
        Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): qualidade trocada no iPhone para \(nova.preset.rawValue, privacy: .public), \(nova.bitrate, privacy: .public) bps")
        fonte.reconfigurar(ConfigVideo(preset: nova.preset, bitrate: nova.bitrate)) { [weak self] r in
            self?.fila.async {
                guard let self, self.fase == .transmitindo else { return }
                switch r {
                case .success(let dim):
                    self.dimAtual = dim
                    self.qualidade = nova
                    self.limiteFila = max(self.opcoes.limiteFilaBytes, nova.bitrate / 8)
                    self.aoMudar?(.conectado(self.clienteNome), "\(dim.largura)x\(dim.altura) a \(dim.fps) fps")
                    self.aoQualidade?(nova)
                case .failure(let e):
                    Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): troca de qualidade falhou: \(e.mensagem, privacy: .public)")
                }
            }
        }
    }

    private func enviarStatus() {
        guard fase == .transmitindo else { return }
        let agora = Date()
        let dt = max(0.001, agora.timeIntervalSince(inicioJanela))
        let fps = (Double(quadrosJanela) / dt * 10).rounded() / 10
        let kbps = Int(Double(bytesJanela) * 8 / dt / 1000)
        quadrosJanela = 0
        bytesJanela = 0
        inicioJanela = agora
        let s = sensores.ler()
        let aj = ajustes.valor
        let leitura = ajustes.leitura
        var obj = StatusCamera.campos(ajustes: aj, leitura: leitura, noAr: ajustes.noAr, dim: dimAtual,
                                  assinaturaVence: ajustes.assinaturaVence)
        obj["bateria"] = s.bateria
        obj["carregando"] = s.carregando
        obj["temp"] = temperaturaTexto(ProcessInfo.processInfo.thermalState)
        obj["fps"] = fps
        obj["kbps"] = kbps
        obj["bitrate"] = qualidade.bitrate
        obj["preset_obs"] = qualidade.presetObs
        obj["bitrate_obs"] = qualidade.bitrateObs
        obj["qualidade_obs"] = qualidade.escolhidaNoOBS
        if let json = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]) {
            enviar(Pacote(tipo: TipoPacote.status, tempo: relogioHostMicros(), dados: json))
        }
        aoMetricas?(fps, kbps)
    }

    /// Manda a linha de erro do handshake e fecha depois que ela saiu.
    private func fecharComErro(_ erro: String) {
        guard fase != .encerrando, fase != .encerrada else { return }
        Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): respondendo erro \(erro, privacy: .public) e fechando")
        fase = .encerrando
        conexao.send(content: Handshake.respostaErro(erro), completion: .contentProcessed { [weak self] _ in
            self?.fila.async { self?.encerrar("fechada após erro \(erro)") }
        })
    }

    func encerrar(_ motivo: String) {
        guard fase != .encerrada else { return }
        Log.servidor.notice("fmcam servidor: sessao \(self.numero, privacy: .public): encerrando na fase \(String(describing: self.fase), privacy: .public), motivo: \(motivo, privacy: .public)")
        fase = .encerrada
        timers.forEach { $0.cancel() }
        timers = []
        // aoEvento não é zerado aqui: o codificador o lê de outra thread. Eventos tardios caem no guard de fase.
        fonte.parar()
        conexao.cancel()
        aoEncerrar?(erroCamera)
    }
}


// MARK: - Cliente recusado (ocupado)

private final class Rejeicao {
    let conexao: NWConnection
    let fila: DispatchQueue
    let timeout: TimeInterval
    private var buffer = Data()
    private var feito = false
    private var timer: DispatchSourceTimer?

    init(conexao: NWConnection, fila: DispatchQueue, timeout: TimeInterval) {
        self.conexao = conexao
        self.fila = fila
        self.timeout = timeout
    }

    func iniciar() {
        conexao.start(queue: fila)
        let t = DispatchSource.makeTimerSource(queue: fila)
        t.schedule(deadline: .now() + timeout)
        t.setEventHandler { [self] in responder() }
        t.resume()
        timer = t
        ler()
    }

    private func ler() {
        conexao.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [self] dados, _, fim, erro in
            guard !feito else { return }
            if let d = dados { buffer.append(d) }
            if buffer.contains(0x0A) || buffer.count > Protocolo.limiteHandshake { responder(); return }
            if fim || erro != nil { encerrar(); return }
            ler()
        }
    }

    private func responder() {
        guard !feito else { return }
        feito = true
        timer?.cancel()
        conexao.send(content: Handshake.respostaErro("ocupado"), contentContext: .finalMessage, isComplete: true,
                     completion: .contentProcessed { [self] _ in conexao.cancel() })
    }

    private func encerrar() {
        feito = true
        timer?.cancel()
        conexao.cancel()
    }
}
