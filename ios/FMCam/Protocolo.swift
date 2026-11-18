import Foundation

/// Contrato do protocolo OBS Cam Mobile v1 (docs/PROTOCOLO.md). Lado servidor (iPhone).
enum Protocolo {
    static let versao = 1
    static let versoesSuportadas = [1]
    static let portaPadrao: UInt16 = 4848
    static let tamanhoCabecalho = 13
    static let tamanhoMaximoPacote = 8 * 1024 * 1024
    static let limiteHandshake = 4096
}

enum TipoPacote {
    static let config: UInt8 = 1
    static let quadro: UInt8 = 2
    static let status: UInt8 = 3
    static let comando: UInt8 = 4
    static let ping: UInt8 = 9
    static let pong: UInt8 = 10

    static let conhecidos: Set<UInt8> = [1, 2, 3, 4, 9, 10]
}

struct Pacote: Equatable {
    var tipo: UInt8
    var tempo: UInt64
    var dados: Data

    /// Cabeçalho de 13 bytes (tipo, tempo u64, tamanho u32, tudo big-endian) + dados.
    func serializar() -> Data {
        var saida = Data(capacity: Protocolo.tamanhoCabecalho + dados.count)
        saida.append(tipo)
        for i in (0..<8).reversed() {
            saida.append(UInt8((tempo >> UInt64(8 * i)) & 0xFF))
        }
        let tamanho = UInt32(dados.count)
        for i in (0..<4).reversed() {
            saida.append(UInt8((tamanho >> UInt32(8 * i)) & 0xFF))
        }
        saida.append(dados)
        return saida
    }

    /// Dados do quadro: 1 byte de flags (bit 0 = quadro-chave) + H.264 em Annex B.
    static func quadro(tempo: UInt64, chave: Bool, annexB: Data) -> Pacote {
        var dados = Data(capacity: annexB.count + 1)
        dados.append(chave ? 0x01 : 0x00)
        dados.append(annexB)
        return Pacote(tipo: TipoPacote.quadro, tempo: tempo, dados: dados)
    }

    /// Pong: os dados são o `tempo` do ping respondido, u64 big-endian.
    static func pong(tempo: UInt64, eco: UInt64) -> Pacote {
        Pacote(tipo: TipoPacote.pong, tempo: tempo, dados: Pacote.u64(eco))
    }

    static func u64(_ valor: UInt64) -> Data {
        Data((0..<8).reversed().map { UInt8((valor >> UInt64(8 * $0)) & 0xFF) })
    }
}

enum ErroProtocolo: Error, Equatable {
    case tamanhoExcessivo(Int)
}

/// Parser incremental: alimente com os bytes como chegam, recebe os pacotes completos.
/// Tipo desconhecido é pulado (os `tamanho` bytes são descartados), tamanho acima de 8 MiB é erro.
struct ParserPacotes {
    private var buffer = Data()
    private var aPular = 0

    mutating func alimentar(_ novos: Data) throws -> [Pacote] {
        buffer.append(novos)
        var saida: [Pacote] = []
        while true {
            if aPular > 0 {
                let n = min(aPular, buffer.count)
                buffer.removeFirst(n)
                aPular -= n
                if aPular > 0 { break }
                continue
            }
            if buffer.count < Protocolo.tamanhoCabecalho { break }
            let base = buffer.startIndex
            let tipo = buffer[base]
            var tempo: UInt64 = 0
            for i in 1...8 { tempo = (tempo << 8) | UInt64(buffer[base + i]) }
            var tamanho = 0
            for i in 9...12 { tamanho = (tamanho << 8) | Int(buffer[base + i]) }
            if tamanho > Protocolo.tamanhoMaximoPacote {
                throw ErroProtocolo.tamanhoExcessivo(tamanho)
            }
            if !TipoPacote.conhecidos.contains(tipo) {
                buffer.removeFirst(Protocolo.tamanhoCabecalho)
                aPular = tamanho
                continue
            }
            if buffer.count < Protocolo.tamanhoCabecalho + tamanho { break }
            let ini = base + Protocolo.tamanhoCabecalho
            let dados = Data(buffer[ini..<(ini + tamanho)])
            buffer.removeFirst(Protocolo.tamanhoCabecalho + tamanho)
            saida.append(Pacote(tipo: tipo, tempo: tempo, dados: dados))
        }
        return saida
    }
}

// MARK: - Handshake

enum Preset: String, CaseIterable, Codable {
    case p1080_30 = "1080p30"
    case p1080_60 = "1080p60"
    case p1080_120 = "1080p120"
    case p2160_30 = "2160p30"
    case p720_30 = "720p30"
    case p720_60 = "720p60"

    var largura: Int {
        switch self {
        case .p2160_30: return 3840
        case .p1080_30, .p1080_60, .p1080_120: return 1920
        case .p720_30, .p720_60: return 1280
        }
    }
    var altura: Int {
        switch self {
        case .p2160_30: return 2160
        case .p1080_30, .p1080_60, .p1080_120: return 1080
        case .p720_30, .p720_60: return 720
        }
    }
    var fps: Int {
        switch self {
        case .p1080_120: return 120
        case .p1080_60, .p720_60: return 60
        case .p1080_30, .p2160_30, .p720_30: return 30
        }
    }

    /// Bitrate quando o cliente não pede um. 1080p30 e os 720p seguem em 10 Mbps.
    var bitratePadrao: Int {
        switch self {
        case .p2160_30: return 25_000_000
        case .p1080_120: return 20_000_000
        default: return PedidoHandshake.bitratePadrao
        }
    }

    /// Do pedido para baixo: se o aparelho não tem o formato, tenta o próximo da lista.
    var quedas: [Preset] {
        switch self {
        case .p2160_30: return [.p2160_30, .p1080_30, .p720_30]
        case .p1080_120: return [.p1080_120, .p1080_60, .p1080_30, .p720_30]
        case .p1080_60: return [.p1080_60, .p1080_30, .p720_30]
        case .p1080_30: return [.p1080_30, .p720_30]
        case .p720_60: return [.p720_60, .p720_30]
        case .p720_30: return [.p720_30]
        }
    }
}

enum Lente: String, Codable {
    case wide, ultra, tele
}

struct PedidoHandshake: Equatable {
    var v: Int
    var cliente: String
    var preset: Preset
    var lente: Lente
    var bitrate: Int
    /// O cliente escreveu `lente` no handshake (e o valor é válido). Sem isso, `wide` é só o padrão e não manda no iPhone.
    var lenteInformada = false
    /// O OBS escreveu `preset` (válido) no handshake. Sem isso, vale a qualidade salva no iPhone.
    var presetInformado = false
    /// O OBS escreveu `bitrate` (válido) no handshake.
    var bitrateInformado = false

    static func == (a: PedidoHandshake, b: PedidoHandshake) -> Bool {
        a.v == b.v && a.cliente == b.cliente && a.preset == b.preset && a.lente == b.lente && a.bitrate == b.bitrate
    }

    static let bitratePadrao = 10_000_000
    static let faixaBitrate = 1_000_000...50_000_000
}

enum ResultadoHandshake: Equatable {
    case aceito(PedidoHandshake)
    /// `v` do cliente que o iPhone não fala.
    case versaoNaoSuportada(Int)
    case invalido
}

/// JSON estrito (RFC 8259) para o texto do protocolo: UTF-8 válido sem BOM, sem vírgula sobrando, sem chave repetida
/// em nenhum objeto, profundidade limitada. `nil` = inválido.
enum JSONEstrito {
    indirect enum Valor: Equatable {
        case objeto([(String, Valor)])
        case lista([Valor])
        case texto(String)
        /// Texto do número como veio, para saber se é inteiro de verdade.
        case numero(String)
        case booleano(Bool)
        case nulo

        static func == (a: Valor, b: Valor) -> Bool {
            switch (a, b) {
            case (.texto(let x), .texto(let y)): return x == y
            case (.numero(let x), .numero(let y)): return x == y
            case (.booleano(let x), .booleano(let y)): return x == y
            case (.nulo, .nulo): return true
            default: return false
            }
        }
    }

    static let profundidadeMaxima = 32

    static func interpretar(_ dados: Data) -> Valor? {
        let b = [UInt8](dados)
        if b.count >= 3, b[0] == 0xEF, b[1] == 0xBB, b[2] == 0xBF { return nil }
        guard String(data: dados, encoding: .utf8) != nil else { return nil }
        var l = Leitor(b: b)
        l.espacos()
        guard let v = l.valor(), { l.espacos(); return l.i == b.count }() else { return nil }
        return v
    }

    private struct Leitor {
        let b: [UInt8]
        var i = 0
        var profundidade = 0

        mutating func espacos() {
            while i < b.count, b[i] == 0x20 || b[i] == 0x09 || b[i] == 0x0A || b[i] == 0x0D { i += 1 }
        }

        mutating func valor() -> Valor? {
            guard i < b.count else { return nil }
            switch b[i] {
            case UInt8(ascii: "{"): return objeto()
            case UInt8(ascii: "["): return lista()
            case UInt8(ascii: "\""): return texto().map(Valor.texto)
            case UInt8(ascii: "t"): return literal("true") ? .booleano(true) : nil
            case UInt8(ascii: "f"): return literal("false") ? .booleano(false) : nil
            case UInt8(ascii: "n"): return literal("null") ? .nulo : nil
            default: return numero()
            }
        }

        mutating func literal(_ s: String) -> Bool {
            let u = Array(s.utf8)
            guard i + u.count <= b.count, Array(b[i..<(i + u.count)]) == u else { return false }
            i += u.count
            return true
        }

        mutating func objeto() -> Valor? {
            profundidade += 1
            defer { profundidade -= 1 }
            guard profundidade <= JSONEstrito.profundidadeMaxima else { return nil }
            i += 1
            var membros: [(String, Valor)] = []
            var vistas = Set<String>()
            espacos()
            if i < b.count, b[i] == UInt8(ascii: "}") { i += 1; return .objeto(membros) }
            while true {
                espacos()
                guard i < b.count, b[i] == UInt8(ascii: "\""), let chave = texto() else { return nil }
                guard vistas.insert(chave).inserted else { return nil }
                espacos()
                guard i < b.count, b[i] == UInt8(ascii: ":") else { return nil }
                i += 1
                espacos()
                guard let v = valor() else { return nil }
                membros.append((chave, v))
                espacos()
                guard i < b.count else { return nil }
                if b[i] == UInt8(ascii: ",") { i += 1; continue }
                if b[i] == UInt8(ascii: "}") { i += 1; return .objeto(membros) }
                return nil
            }
        }

        mutating func lista() -> Valor? {
            profundidade += 1
            defer { profundidade -= 1 }
            guard profundidade <= JSONEstrito.profundidadeMaxima else { return nil }
            i += 1
            var itens: [Valor] = []
            espacos()
            if i < b.count, b[i] == UInt8(ascii: "]") { i += 1; return .lista(itens) }
            while true {
                espacos()
                guard let v = valor() else { return nil }
                itens.append(v)
                espacos()
                guard i < b.count else { return nil }
                if b[i] == UInt8(ascii: ",") { i += 1; continue }
                if b[i] == UInt8(ascii: "]") { i += 1; return .lista(itens) }
                return nil
            }
        }

        mutating func hex4() -> UInt32? {
            guard i + 4 <= b.count else { return nil }
            var v: UInt32 = 0
            for k in 0..<4 {
                guard let d = Character(UnicodeScalar(b[i + k])).hexDigitValue else { return nil }
                v = v * 16 + UInt32(d)
            }
            i += 4
            return v
        }

        mutating func texto() -> String? {
            i += 1
            var out: [UInt8] = []
            while i < b.count {
                let c = b[i]
                i += 1
                if c == UInt8(ascii: "\"") { return String(decoding: out, as: UTF8.self) }
                if c < 0x20 { return nil }
                if c != UInt8(ascii: "\\") { out.append(c); continue }
                guard i < b.count else { return nil }
                let e = b[i]
                i += 1
                switch e {
                case UInt8(ascii: "\""): out.append(0x22)
                case UInt8(ascii: "\\"): out.append(0x5C)
                case UInt8(ascii: "/"): out.append(0x2F)
                case UInt8(ascii: "b"): out.append(0x08)
                case UInt8(ascii: "f"): out.append(0x0C)
                case UInt8(ascii: "n"): out.append(0x0A)
                case UInt8(ascii: "r"): out.append(0x0D)
                case UInt8(ascii: "t"): out.append(0x09)
                case UInt8(ascii: "u"):
                    guard var u = hex4() else { return nil }
                    if (0xD800...0xDBFF).contains(u) {
                        guard i + 1 < b.count, b[i] == UInt8(ascii: "\\"), b[i + 1] == UInt8(ascii: "u") else { return nil }
                        i += 2
                        guard let baixo = hex4(), (0xDC00...0xDFFF).contains(baixo) else { return nil }
                        u = 0x10000 + ((u - 0xD800) << 10) + (baixo - 0xDC00)
                    } else if (0xDC00...0xDFFF).contains(u) {
                        return nil
                    }
                    guard let esc = UnicodeScalar(u) else { return nil }
                    out.append(contentsOf: Array(String(Character(esc)).utf8))
                default: return nil
                }
            }
            return nil
        }

        mutating func numero() -> Valor? {
            let ini = i
            if i < b.count, b[i] == UInt8(ascii: "-") { i += 1 }
            guard i < b.count else { return nil }
            if b[i] == UInt8(ascii: "0") { i += 1 }
            else if (UInt8(ascii: "1")...UInt8(ascii: "9")).contains(b[i]) { while i < b.count, digito(b[i]) { i += 1 } }
            else { return nil }
            if i < b.count, b[i] == UInt8(ascii: ".") {
                i += 1
                guard i < b.count, digito(b[i]) else { return nil }
                while i < b.count, digito(b[i]) { i += 1 }
            }
            if i < b.count, b[i] == UInt8(ascii: "e") || b[i] == UInt8(ascii: "E") {
                i += 1
                if i < b.count, b[i] == UInt8(ascii: "+") || b[i] == UInt8(ascii: "-") { i += 1 }
                guard i < b.count, digito(b[i]) else { return nil }
                while i < b.count, digito(b[i]) { i += 1 }
            }
            return .numero(String(decoding: b[ini..<i], as: UTF8.self))
        }

        func digito(_ c: UInt8) -> Bool { c >= 0x30 && c <= 0x39 }
    }
}

enum Handshake {
    /// `linha` sem o `\n` final. Regras do texto em docs/PROTOCOLO.md: JSON estrito, chave repetida e BOM são `handshake`,
    /// `v` precisa ser inteiro (fora das versões suportadas é `versao`), campo desconhecido de qualquer tipo é ignorado,
    /// valor inválido em campo opcional cai no padrão.
    static func interpretar(linha: Data) -> ResultadoHandshake {
        if linha.count > Protocolo.limiteHandshake { return .invalido }
        guard case .objeto(let membros)? = JSONEstrito.interpretar(linha) else { return .invalido }
        func campo(_ nome: String) -> JSONEstrito.Valor? { membros.first { $0.0 == nome }?.1 }

        guard case .numero(let lexema)? = campo("v"), ehInteiro(lexema) else { return .invalido }
        guard let v = Int(lexema) else {
            return .versaoNaoSuportada(lexema.hasPrefix("-") ? Int.min : Int.max)
        }
        if !Protocolo.versoesSuportadas.contains(v) { return .versaoNaoSuportada(v) }

        var cliente = ""
        if case .texto(let c)? = campo("cliente") { cliente = c }
        var preset = Preset.p1080_30
        var presetInformado = false
        if case .texto(let t)? = campo("preset"), let p = Preset(rawValue: t) { preset = p; presetInformado = true }
        var lente = Lente.wide
        var lenteInformada = false
        if case .texto(let t)? = campo("lente"), let l = Lente(rawValue: t) { lente = l; lenteInformada = true }
        var bitrate = preset.bitratePadrao
        var bitrateInformado = false
        if case .numero(let n)? = campo("bitrate"), let d = Double(n), d == d.rounded(), d.isFinite,
           PedidoHandshake.faixaBitrate.contains(Int(exactly: d) ?? -1) {
            bitrate = Int(d)
            bitrateInformado = true
        }
        return .aceito(PedidoHandshake(v: v, cliente: cliente, preset: preset, lente: lente, bitrate: bitrate, lenteInformada: lenteInformada,
                                       presetInformado: presetInformado, bitrateInformado: bitrateInformado))
    }

    /// Inteiro de verdade: só dígitos (com sinal opcional), sem ponto nem expoente.
    private static func ehInteiro(_ lexema: String) -> Bool {
        !lexema.contains(".") && !lexema.contains("e") && !lexema.contains("E")
    }

    static func respostaOk(aparelho: String, nome: String, id: String, largura: Int, altura: Int, fps: Int) -> Data {
        linha(["ok": true, "v": Protocolo.versao, "aparelho": aparelho, "nome": nome, "id": id,
               "w": largura, "h": altura, "fps": fps, "codec": "h264"])
    }

    /// `erro`: "versao", "ocupado", "handshake" ou "camera".
    static func respostaErro(_ erro: String) -> Data {
        var obj: [String: Any] = ["ok": false, "erro": erro]
        if erro == "versao" { obj["v_suportadas"] = Protocolo.versoesSuportadas }
        return linha(obj)
    }

    private static func linha(_ obj: [String: Any]) -> Data {
        var d = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data("{}".utf8)
        d.append(0x0A)
        return d
    }
}
