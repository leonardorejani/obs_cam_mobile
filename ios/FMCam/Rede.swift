import Foundation
import Network

struct InterfaceRede: Equatable {
    var nome: String
    var ipv4: String
}

enum OrigemConexao: String, Equatable {
    case cabo = "Cabo"
    case wifi = "Wi-Fi"
}

enum EnderecoRede {
    /// Interface do Wi-Fi no iPhone é a en0. Endereço link-local (169.254) não vale: é rede sem DHCP.
    static func escolherWifi(_ itens: [InterfaceRede]) -> String? {
        itens.first { $0.nome == "en0" && !$0.ipv4.isEmpty && !$0.ipv4.hasPrefix("169.254.") && !$0.ipv4.hasPrefix("127.") }?.ipv4
    }

    static func formatar(ip: String?, porta: UInt16) -> String {
        guard let ip else { return "Wi-Fi: desligado (use o cabo)" }
        return "Wi-Fi: \(ip):\(porta)"
    }

    /// Só o endereço, para copiar.
    static func enderecoParaCopiar(ip: String?, porta: UInt16) -> String? {
        ip.map { "\($0):\(porta)" }
    }

    /// Conexão vinda de loopback é o cabo (usbmux entrega no próprio aparelho). Qualquer outro endereço é a rede local.
    static func origem(host: String) -> OrigemConexao {
        let h = host.split(separator: "%").first.map(String.init)?.lowercased() ?? host.lowercased()
        if h == "::1" || h == "localhost" || h.hasPrefix("127.") || h.hasPrefix("::ffff:127.") { return .cabo }
        return .wifi
    }

    static func origem(de endpoint: NWEndpoint) -> OrigemConexao? {
        guard case .hostPort(let host, _) = endpoint else { return nil }
        switch host {
        case .ipv4(let a): return origem(host: "\(a)")
        case .ipv6(let a): return origem(host: "\(a)")
        case .name(let n, _): return origem(host: n)
        @unknown default: return nil
        }
    }

    /// IPv4 de cada interface ativa (getifaddrs).
    static func listar() -> [InterfaceRede] {
        var inicio: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&inicio) == 0, let primeiro = inicio else { return [] }
        defer { freeifaddrs(inicio) }
        var saida: [InterfaceRede] = []
        var p: UnsafeMutablePointer<ifaddrs>? = primeiro
        while let i = p {
            let f = i.pointee
            if let addr = f.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
               (Int32(f.ifa_flags) & IFF_UP) != 0 {
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                    saida.append(InterfaceRede(nome: String(cString: f.ifa_name), ipv4: String(cString: host)))
                }
            }
            p = f.ifa_next
        }
        return saida
    }
}

enum Bonjour {
    static let tipo = "_fmcam._tcp"
    /// Limite do nome de serviço do Bonjour.
    static let tamanhoMaximoNome = 63

    static func txt(id: String) -> [String: String] {
        ["id": id, "v": String(Protocolo.versao)]
    }

    static func nomeServico(_ nome: String) -> String {
        let limpo = nome.trimmingCharacters(in: .whitespacesAndNewlines)
        return String((limpo.isEmpty ? "OBS Cam Mobile" : limpo).prefix(tamanhoMaximoNome))
    }

    static func servico(nome: String, id: String) -> NWListener.Service {
        NWListener.Service(name: nomeServico(nome), type: tipo, domain: nil, txtRecord: NWTXTRecord(txt(id: id)))
    }
}
