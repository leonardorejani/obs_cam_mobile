#include <unistd.h>

#include <atomic>
#include <climits>
#include <chrono>
#include <thread>

#include "teste.h"
#include "usbmux.h"
#include "usbmuxd_falso.h"

using namespace fmappcam;

namespace {

std::string caminhoTemp(const char* marca) {
  static int n = 0;
  return "/tmp/fmcam_" + std::string(marca) + "_" + std::to_string(getpid()) + "_" + std::to_string(n++) + ".sock";
}

// Servidor que devolve em eco, com um prefixo para saber quem respondeu.
struct Eco {
  Soquete escuta = kSoqueteInvalido;
  uint16_t porta = 0;
  std::string prefixo;
  std::atomic<bool> parar{false};
  std::thread th;
  explicit Eco(std::string p) : prefixo(std::move(p)) {
    std::string erro;
    escuta = escutarTcp(porta, erro);
    th = std::thread([this] {
      std::vector<std::thread> filhos;
      while (!parar) {
        Soquete c = aceitar(escuta, 50);
        if (c == kSoqueteInvalido) continue;
        filhos.emplace_back([this, c] {
          uint8_t b[256];
          enviarTudo(c, prefixo.data(), prefixo.size());
          for (;;) {
            long n = lerAlgum(c, b, sizeof b);
            if (n <= 0) break;
            enviarTudo(c, b, size_t(n));
          }
          fecharSoquete(c);
        });
      }
      for (auto& f : filhos) f.join();
    });
  }
  ~Eco() {
    parar = true;
    th.join();
    fecharSoquete(escuta);
  }
};

bool esperarAte(const std::function<bool()>& f, int ms = 3000) {
  for (int i = 0; i < ms / 10; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return f();
}

}  // namespace

TESTE(plist_connect_tem_porta_em_ordem_de_rede) {
  std::string x = montarPlistConnect(7, 4848);
  CHECAR(x.find("<integer>61458</integer>") != std::string::npos);  // htons(4848)
  CHECAR(x.find("<key>PortNumber</key>") != std::string::npos);
  CHECAR(x.find("<string>fmcam</string>") != std::string::npos);  // ProgName e ClientVersionString
  NoPlist p;
  CHECAR(lerPlist(x, p));
  int64_t n = 0;
  CHECAR(p.buscarInteiro("DeviceID", n) && n == 7);
  CHECAR(p.buscarInteiro("PortNumber", n) && n == 61458);
  std::string t;
  CHECAR(p.buscarTexto("MessageType", t) && t == "Connect");
  CHECAR(p.buscarTexto("ProgName", t) && t == "fmcam");
}

TESTE(mensagem_usbmux_cabecalho_little_endian) {
  auto m = montarMensagemUsbmux(0x01020304, "abc");
  CHECAR_IGUAIS(m.size(), size_t(19));
  uint8_t esperado[16] = {19, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 4, 3, 2, 1};
  CHECAR(std::equal(esperado, esperado + 16, m.begin()));
  uint32_t tam, ver, tipo, tag;
  CHECAR(lerCabecalhoUsbmux(m.data(), tam, ver, tipo, tag));
  CHECAR(tam == 19 && ver == 1 && tipo == 8 && tag == 0x01020304);
}

TESTE(plist_ler_resposta_result) {
  // como o usbmuxd de verdade responde
  const char* xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>MessageType</key>\n"
      "\t<string>Result</string>\n\t<key>Number</key>\n\t<integer>0</integer>\n</dict>\n</plist>\n";
  int n = -1;
  CHECAR(lerResultUsbmux(xml, n));
  CHECAR_IGUAIS(n, 0);
  CHECAR(lerResultUsbmux(montarPlistResult(3), n) && n == 3);
  CHECAR(!lerResultUsbmux("<plist><dict></dict></plist>", n));
  CHECAR(!lerResultUsbmux("lixo", n));
}

TESTE(plist_ler_device_list_filtra_usb) {
  const char* xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n<key>DeviceList</key>\n<array>\n"
      "<dict><key>DeviceID</key><integer>1</integer><key>MessageType</key><string>Attached</string>"
      "<key>Properties</key><dict><key>ConnectionType</key><string>USB</string><key>DeviceID</key><integer>1</integer>"
      "<key>LocationID</key><integer>1234</integer><key>ProductID</key><integer>4776</integer>"
      "<key>SerialNumber</key><string>00008110-AAAA</string></dict></dict>\n"
      "<dict><key>DeviceID</key><integer>2</integer><key>MessageType</key><string>Attached</string>"
      "<key>Properties</key><dict><key>ConnectionType</key><string>Network</string><key>DeviceID</key><integer>2</integer>"
      "<key>EscapedFullServiceName</key><string>a&amp;b._apple-mobdev2._tcp.local.</string>"
      "<key>SerialNumber</key><string>00008110-BBBB</string></dict></dict>\n"
      "<dict><key>DeviceID</key><integer>3</integer><key>MessageType</key><string>Attached</string>"
      "<key>Properties</key><dict><key>ConnectionType</key><string>USB</string><key>DeviceID</key><integer>3</integer>"
      "<key>SerialNumber</key><string>00008110-CCCC</string></dict></dict>\n</array>\n</dict>\n</plist>\n";
  std::vector<DispositivoUsb> v;
  CHECAR(lerListaDispositivos(xml, v));
  CHECAR_IGUAIS(v.size(), size_t(2));
  CHECAR(v[0].id == 1 && v[0].serial == "00008110-AAAA");
  CHECAR(v[1].id == 3 && v[1].serial == "00008110-CCCC");
  CHECAR(lerListaDispositivos(xml, v, false) && v.size() == 3);
  CHECAR(v[1].conexao == "Network");
  CHECAR(!lerListaDispositivos("<plist><dict><key>Outra</key><string>x</string></dict></plist>", v));
}

TESTE(plist_roundtrip_lista) {
  std::vector<DispositivoUsb> a = {{4, "UDID-4", "USB"}, {9, "UDID&9", "Network"}};
  std::vector<DispositivoUsb> b;
  CHECAR(lerListaDispositivos(montarPlistDeviceList(a), b, false));
  CHECAR(a == b);
}

TESTE(plist_lixo_nao_quebra) {
  NoPlist p;
  std::string base = montarPlistConnect(3, 4848);
  for (size_t n = 0; n < base.size(); n += 3) lerPlist(base.substr(0, n), p);
  CHECAR(!lerPlist("", p));
  CHECAR(!lerPlist("<plist><dict><key>a</key></dict></plist>", p));
  std::string fundo = "<plist>";
  for (int i = 0; i < 100; ++i) fundo += "<array>";
  CHECAR(!lerPlist(fundo, p));
}

TESTE(cliente_contra_usbmuxd_falso) {
  Eco eco("A:");
  std::string caminho = caminhoTemp("um");
  UsbmuxdFalso fake(caminho);
  fake.anexar({3, "UDID-REDE", "Network"}, "127.0.0.1", 1);
  fake.anexar({7, "UDID-USB-7", "USB"}, "127.0.0.1", eco.porta);
  std::string erro;
  CHECAR(fake.iniciar(erro));

  std::vector<DispositivoUsb> lista;
  CHECAR(listarDispositivosUsb(caminho, lista, erro));
  CHECAR_IGUAIS(lista.size(), size_t(1));  // so USB
  CHECAR(lista[0].id == 7 && lista[0].serial == "UDID-USB-7");

  Soquete s = conectarDispositivoUsb(caminho, 7, 4848, erro);
  CHECAR(s != kSoqueteInvalido);
  if (s != kSoqueteInvalido) {
    CHECAR_IGUAIS(fake.portaRecebida(), 61458);
    uint8_t b[2];
    CHECAR(lerExato(s, b, 2, 2000) == ResultadoIO::Ok);  // prefixo do destino, ja pelo tunel
    CHECAR(b[0] == 'A' && b[1] == ':');
    CHECAR(enviarTudo(s, "oi", 2));
    CHECAR(lerExato(s, b, 2, 2000) == ResultadoIO::Ok);
    CHECAR(b[0] == 'o' && b[1] == 'i');
    fecharSoquete(s);
  }
  // aparelho que nao existe e porta errada
  CHECAR(conectarDispositivoUsb(caminho, 99, 4848, erro) == kSoqueteInvalido);
  CHECAR(conectarDispositivoUsb(caminho, 7, 5000, erro) == kSoqueteInvalido);
  CHECAR(erro.find("recusou") != std::string::npos);
  fake.parar();
}

TESTE(cliente_sem_usbmuxd_da_erro_limpo) {
  std::string erro;
  std::vector<DispositivoUsb> l;
  CHECAR(!listarDispositivosUsb("/tmp/nao_existe_fmcam.sock", l, erro));
  CHECAR(!erro.empty());
}

TESTE(varios_aparelhos_cada_um_no_seu_destino) {
  Eco a("A:"), b("B:"), c("C:"), d("D:");
  Eco* ecos[4] = {&a, &b, &c, &d};
  std::string caminho = caminhoTemp("multi");
  UsbmuxdFalso fake(caminho);
  for (int i = 0; i < 4; ++i)
    fake.anexar({10 + i, "UDID-" + std::to_string(i), "USB"}, "127.0.0.1", ecos[i]->porta);
  std::string erro;
  CHECAR(fake.iniciar(erro));
  std::vector<DispositivoUsb> lista;
  CHECAR(listarDispositivosUsb(caminho, lista, erro));
  CHECAR_IGUAIS(lista.size(), size_t(4));
  // conecta nos 4 ao mesmo tempo, por UDID, fora de ordem
  const int ordem[4] = {2, 0, 3, 1};
  Soquete s[4];
  for (int k = 0; k < 4; ++k) s[k] = conectarUsbPorUdid(caminho, "UDID-" + std::to_string(ordem[k]), 4848, erro);
  for (int k = 0; k < 4; ++k) {
    CHECAR(s[k] != kSoqueteInvalido);
    if (s[k] == kSoqueteInvalido) continue;
    uint8_t buf[2];
    CHECAR(lerExato(s[k], buf, 2, 2000) == ResultadoIO::Ok);
    CHECAR_IGUAIS(char(buf[0]), char('A' + ordem[k]));
  }
  CHECAR(conectarUsbPorUdid(caminho, "UDID-INEXISTENTE", 4848, erro) == kSoqueteInvalido);
  for (int k = 0; k < 4; ++k) fecharSoquete(s[k]);
  fake.parar();
}

TESTE(listen_acompanha_plugar_e_desplugar) {
  std::string caminho = caminhoTemp("listen");
  UsbmuxdFalso fake(caminho);
  fake.anexar({1, "UDID-1", "USB"}, "127.0.0.1", 1);
  std::string erro;
  CHECAR(fake.iniciar(erro));

  std::atomic<int> avisos{0};
  ObservadorUsbmux obs(caminho);
  obs.iniciar([&] { ++avisos; });
  CHECAR(esperarAte([&] { return obs.dispositivos().size() == 1; }));  // ja plugado ao ouvir
  CHECAR(obs.ouvindo());

  fake.anexar({2, "UDID-2", "USB"}, "127.0.0.1", 1);
  fake.anexar({3, "UDID-REDE", "Network"}, "127.0.0.1", 1);  // rede nao entra
  CHECAR(esperarAte([&] { return obs.dispositivos().size() == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECAR_IGUAIS(obs.dispositivos().size(), size_t(2));

  fake.desanexar(1);
  CHECAR(esperarAte([&] { return obs.dispositivos().size() == 1; }));
  CHECAR_IGUAIS(obs.dispositivos()[0].serial, std::string("UDID-2"));

  fake.anexar({4, "UDID-4", "USB"}, "127.0.0.1", 1);
  CHECAR(esperarAte([&] { return obs.dispositivos().size() == 2; }));
  CHECAR(avisos >= 4);

  // usbmuxd some: a lista esvazia, e volta sozinha quando ele reaparecer
  fake.parar();
  CHECAR(esperarAte([&] { return obs.dispositivos().empty(); }));
  CHECAR(!obs.ouvindo());
  obs.parar();
}

TESTE(result_usbmux_nao_trunca_64_bits) {
  auto result = [](const char* num) {
    return std::string("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>MessageType</key><string>Result</string><key>Number</key><integer>") +
           num + "</integer></dict></plist>";
  };
  int n = -99;
  CHECAR(lerResultUsbmux(result("0"), n) && n == 0);
  CHECAR(lerResultUsbmux(result("3"), n) && n == 3);
  CHECAR(lerResultUsbmux(result("-2147483648"), n) && n == INT_MIN);
  // 2^32 virava 0 (= conectado). Fora da faixa de int a resposta e invalida.
  for (const char* x : {"4294967296", "4294967297", "-4294967296", "2147483648", "9223372036854775807"}) {
    n = -99;
    CHECAR(!lerResultUsbmux(result(x), n));
    CHECAR_IGUAIS(n, -99);
  }
}

TESTE(device_id_gigante_nao_vira_outro_aparelho) {
  std::string xml = montarPlistDeviceList({{1, "A", "USB"}});
  std::string alvo = "<integer>1</integer>";
  size_t p = xml.find(alvo);
  CHECAR(p != std::string::npos);
  xml.replace(p, alvo.size(), "<integer>4294967297</integer>");  // 2^32 + 1 truncava para 1
  std::vector<DispositivoUsb> v;
  CHECAR(lerListaDispositivos(xml, v));
  for (auto& d : v) CHECAR(d.id != 1);
}

TESTE(escolher_dispositivo_usb_primeiro_e_rede_do_mesmo_udid_como_reserva) {
  std::vector<DispositivoUsb> l = {{1, "A", "Network"}, {2, "A", "USB"}, {3, "B", "Network"}};
  DispositivoUsb d;
  CHECAR(escolherDispositivo(l, "A", d) && d.id == 2 && d.conexao == "USB");  // USB tem prioridade, mesmo depois da rede
  CHECAR(escolherDispositivo(l, "", d) && d.id == 2);                         // sem UDID: primeiro USB
  CHECAR(escolherDispositivo(l, "B", d) && d.id == 3 && d.conexao == "Network");  // so rede: serve
  CHECAR(!escolherDispositivo(l, "C", d));                                    // outro UDID nao serve
  std::vector<DispositivoUsb> soRede = {{3, "B", "Network"}};
  CHECAR(!escolherDispositivo(soRede, "", d));  // sem UDID, rede sozinha nao e "iPhone no cabo"
  CHECAR(!escolherDispositivo({}, "A", d));
}

TESTE(lista_completa_e_observador_veem_a_entrada_de_rede) {
  std::string caminho = caminhoTemp("rede");
  UsbmuxdFalso fake(caminho);
  fake.anexar({3, "UDID-REDE", "Network"}, "127.0.0.1", 1);
  fake.anexar({7, "UDID-7", "USB"}, "127.0.0.1", 1);
  std::string erro;
  CHECAR(fake.iniciar(erro));
  std::vector<DispositivoUsb> todos;
  CHECAR(listarDispositivos(caminho, todos, erro));
  CHECAR_IGUAIS(todos.size(), size_t(2));
  ObservadorUsbmux obs(caminho);
  obs.iniciar();
  CHECAR(esperarAte([&] { return obs.todos().size() == 2; }));
  CHECAR_IGUAIS(obs.dispositivos().size(), size_t(1));  // dispositivos() segue so USB
  fake.desanexar(7);  // o macOS apaga a USB e a rede fica
  CHECAR(esperarAte([&] { return obs.dispositivos().empty() && obs.todos().size() == 1; }));
  obs.parar();
  fake.parar();
}
