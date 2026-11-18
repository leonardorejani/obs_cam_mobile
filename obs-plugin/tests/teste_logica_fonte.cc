// Logica da fonte do OBS (sem libobs) contra iphone-falso + usbmuxd-falso:
// 4 aparelhos ao mesmo tempo, nome/modelo vindos do handshake, um aparelho cai e volta, dois fontes no mesmo
// iPhone, aparelho sem app, e a conversao AVFrame para o quadro do OBS.
// Variaveis de ambiente: FMCAM_IPHONE_FALSO, FMCAM_AMOSTRA.
#include <unistd.h>

#include <atomic>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "aparelhos.h"
#include "ajuste_itens.h"
#include "config_fonte.h"
#include "modelo_iphone.h"
#include "proc.h"
#include "quadro.h"
#include "sessao.h"
#include "teste.h"
#include "usbmuxd_falso.h"

extern "C" {
#include <libavutil/frame.h>
}
#ifdef __APPLE__
#include <arpa/inet.h>
#include <dns_sd.h>
#endif

using namespace fmcam;
using namespace fmappcam;
using namespace std::chrono_literals;

namespace {

std::string env(const char* k) {
  const char* v = std::getenv(k);
  return v ? v : "";
}

std::string sockTemp(const char* m) { return "/tmp/fmcam_" + std::string(m) + "_" + std::to_string(getpid()) + ".sock"; }

template <class F>
bool esperar(F cond, int ms = 8000) {
  for (int i = 0; i < ms / 20; ++i) {
    if (cond()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return cond();
}

struct Contador {
  std::atomic<int> quadros{0};
  std::atomic<int> w{0}, h{0};
  std::atomic<int> formato{0};
  SessaoFonte::AoQuadro cb() {
    return [this](const QuadroSaida& q) {
      ++quadros;
      w = q.largura;
      h = q.altura;
      formato = static_cast<int>(q.formato);
    };
  }
};

struct Cenario {
  std::string sock = sockTemp("logica");
  UsbmuxdFalso fake{sock};
  std::vector<fmtest::Proc> iphones;
  std::vector<uint16_t> portas;
  Cenario() {
    std::string erro;
    if (!fake.iniciar(erro)) {
      std::fprintf(stderr, "usbmuxd falso: %s\n", erro.c_str());
      std::exit(97);
    }
  }
  ~Cenario() {
    for (auto& p : iphones) p.matar();
    fake.parar();
  }
  void ligar(int i, int idDev) {  // iPhone i (1..), DeviceID idDev
    uint16_t porta;
    auto p = fmtest::iphoneFalso(env("FMCAM_IPHONE_FALSO"), env("FMCAM_AMOSTRA"), "Camera " + std::to_string(i),
                                 "iPhone " + std::to_string(10 + i), "uuid-" + std::to_string(i), porta);
    if (iphones.size() < static_cast<size_t>(i)) {
      iphones.resize(i);
      portas.resize(i);
    }
    iphones[i - 1] = std::move(p);
    portas[i - 1] = porta;
    fake.anexar({idDev, "UDID-" + std::to_string(i), "USB"}, "127.0.0.1", porta);
  }
};

ConfigFonte cfgPara(const std::string& udid) {
  ConfigFonte c;
  c.udid = udid;
  return c;
}

}  // namespace

TESTE(quatro_iphones_ao_mesmo_tempo_e_um_cai_e_volta) {
  Cenario c;
  for (int i = 1; i <= 4; ++i) c.ligar(i, i);
  RegistroAparelhos reg(c.sock);
  std::atomic<int> avisos{0};
  reg.assinar([&] { ++avisos; });
  reg.iniciar();
  CHECAR(esperar([&] { return reg.listar().size() == 4; }));
  CHECAR(avisos > 0);

  Contador cont[4];
  std::vector<std::unique_ptr<SessaoFonte>> s;
  for (int i = 0; i < 4; ++i) {
    s.push_back(std::make_unique<SessaoFonte>(cfgPara("UDID-" + std::to_string(i + 1)), c.sock, &reg));
    s[i]->definirCallbacks(cont[i].cb(), nullptr);
    s[i]->iniciar();
  }
  CHECAR(esperar([&] {
    for (auto& x : cont)
      if (x.quadros < 30) return false;
    return true;
  }));
  for (int i = 0; i < 4; ++i) {
    auto e = s[i]->estado();
    CHECAR(e.estado == EstadoAparelho::Conectado);
    CHECAR_IGUAIS(e.nome, "Camera " + std::to_string(i + 1));
    CHECAR_IGUAIS(e.modelo, "iPhone " + std::to_string(11 + i));
    CHECAR_IGUAIS(cont[i].w.load(), 1920);
    CHECAR_IGUAIS(cont[i].h.load(), 1080);
    CHECAR(e.errosDecodificacao == 0);
  }
  // lista mostra nome e modelo de cada um, pelo handshake
  auto lista = reg.listar();
  CHECAR_IGUAIS(lista.size(), size_t(4));
  for (auto& i : lista) {
    CHECAR(i.estado == EstadoAparelho::Conectado);
    CHECAR(i.noCabo);
    CHECAR(rotuloAparelho(i).find("Camera ") == 0);
  }

  // o aparelho 3 sai do cabo: os outros nao sentem
  int antes[4];
  for (int i = 0; i < 4; ++i) antes[i] = cont[i].quadros;
  c.iphones[2].matar();
  c.fake.desanexar(3);
  CHECAR(esperar([&] { return reg.listar().size() == 3 || s[2]->estado().estado != EstadoAparelho::Conectado; }));
  CHECAR(esperar([&] {
    InfoAparelho i;
    return reg.buscar("UDID-3", i) && i.estado == EstadoAparelho::SemCabo;
  }));
  CHECAR(s[2]->estado().jaTeveQuadro);  // segura o ultimo quadro (a fonte nao zera nada)
  std::this_thread::sleep_for(1500ms);
  for (int i : {0, 1, 3}) {
    CHECAR(cont[i].quadros > antes[i] + 20);
    CHECAR(s[i]->estado().estado == EstadoAparelho::Conectado);
  }
  int paradoEm = cont[2].quadros;

  // volta: iPhone novo, DeviceID novo, mesmo UDID. A fonte reconecta sozinha.
  c.ligar(3, 7);
  CHECAR(esperar([&] { return cont[2].quadros > paradoEm + 30; }, 12000));
  CHECAR(s[2]->estado().estado == EstadoAparelho::Conectado);
  CHECAR(s[2]->estado().reconexoes >= 1);

  // fechar tudo nao prende
  auto t0 = std::chrono::steady_clock::now();
  for (auto& x : s) CHECAR(x->parar(3000));
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECAR(ms < 3000);
  CHECAR(aguardarEncerramentos(3000));
  InfoAparelho i;
  CHECAR(reg.buscar("UDID-1", i) && i.estado == EstadoAparelho::SemFonte);  // soltou os aparelhos
  reg.parar();
}

TESTE(dois_fontes_no_mesmo_iphone_o_segundo_fica_ocupado) {
  Cenario c;
  c.ligar(1, 1);
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  CHECAR(esperar([&] { return reg.listar().size() == 1; }));
  Contador a, b;
  SessaoFonte s1(cfgPara("UDID-1"), c.sock, &reg), s2(cfgPara("UDID-1"), c.sock, &reg);
  s1.definirCallbacks(a.cb(), nullptr);
  s2.definirCallbacks(b.cb(), nullptr);
  s1.iniciar();
  CHECAR(esperar([&] { return a.quadros > 10; }));
  s2.iniciar();
  CHECAR(esperar([&] { return s2.estado().estado == EstadoAparelho::Ocupado; }));
  CHECAR(s2.estado().mensagem.find("outra fonte") != std::string::npos);
  std::this_thread::sleep_for(500ms);
  CHECAR_IGUAIS(b.quadros.load(), 0);
  CHECAR(s1.estado().estado == EstadoAparelho::Conectado);
  // a primeira solta: a segunda assume sozinha
  CHECAR(s1.parar(3000));
  CHECAR(esperar([&] { return b.quadros > 10; }, 12000));
  CHECAR(s2.estado().estado == EstadoAparelho::Conectado);
  s2.parar(3000);
  reg.parar();
}

TESTE(iphone_ocupado_por_outro_computador_e_app_fechado) {
  Cenario c;
  c.ligar(1, 1);
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  // "outro computador": uma conexao crua segura o iPhone
  Conexao intruso([&] {
    OpcoesConexao o;
    o.usb = true;
    o.usbmuxCaminho = c.sock;
    o.udid = "UDID-1";
    return o;
  }());
  intruso.iniciar();
  CHECAR(esperar([&] { return intruso.conectada(); }));
  Contador cont;
  SessaoFonte s(cfgPara("UDID-1"), c.sock, &reg);
  s.definirCallbacks(cont.cb(), nullptr);
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::Ocupado; }));
  CHECAR(s.estado().mensagem.find("outro computador") != std::string::npos);
  intruso.parar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::Conectado && cont.quadros > 5; }, 12000));
  // app fechado: iphone morre, o cabo continua
  c.iphones[0].matar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::SemApp; }, 10000));
  CHECAR(s.estado().jaTeveQuadro);
  s.parar(3000);
  reg.parar();
}

TESTE(config_primeiro_iphone_aprende_o_udid) {
  Cenario c;
  c.ligar(2, 5);
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  Contador a;
  SessaoFonte s(ConfigFonte{}, c.sock, &reg);  // sem UDID
  s.definirCallbacks(a.cb(), nullptr);
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::Conectado; }));
  CHECAR_IGUAIS(s.estado().udid, "UDID-2");
  s.parar(3000);
  reg.parar();
}

TESTE(sessao_parada_com_usbmuxd_morto_nao_trava) {
  RegistroAparelhos reg("/tmp/fmcam_nao_existe.sock");
  reg.iniciar();
  SessaoFonte s(cfgPara("UDID-X"), "/tmp/fmcam_nao_existe.sock", &reg);
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::SemCabo || s.estado().estado == EstadoAparelho::Procurando; }, 3000));
  auto t0 = std::chrono::steady_clock::now();
  CHECAR(s.parar(2000));
  CHECAR(std::chrono::steady_clock::now() - t0 < 2s);
  reg.parar();
}

TESTE(config_normaliza_e_monta_opcoes) {
  ConfigFonte c;
  c.preset = "4k";  // invalida: cai para "definida no iPhone"
  c.lente = "x";  // invalida: cai para "manter a do iPhone", nunca para wide
  c.bitrateKbps = 999999;
  c.wifiIp = "  192.168.0.5 ";
  c = normalizar(c);
  CHECAR(c.preset.empty());
  CHECAR(c.lente.empty());
  CHECAR_IGUAIS(c.bitrateKbps, 50000);
  CHECAR_IGUAIS(c.wifiIp, "192.168.0.5");
  c.preset = "720p60";
  c.udid = "ABC";
  auto o = montarOpcoes(c, "/x");
  CHECAR(o.usb);
  CHECAR_IGUAIS(o.udid, "ABC");
  CHECAR_IGUAIS(o.pedido.preset, "720p60");
  CHECAR_IGUAIS(o.pedido.bitrate, int64_t(50000000));
  c.wifi = true;
  o = montarOpcoes(c, "/x");
  CHECAR(!o.usb);
  CHECAR_IGUAIS(o.host, "192.168.0.5");
  ConfigFonte d = c;
  d.nomeCache = "outro";
  CHECAR(!mudouConexao(c, d));
  d.lente = "tele";
  CHECAR(mudouConexao(c, d));
  CHECAR_IGUAIS(udidCurto("00008130-001234567890001C"), "00008130..001C");
  InfoAparelho i;
  i.udid = "00008130-001234567890001C";
  CHECAR_IGUAIS(rotuloAparelho(i), "iPhone 00008130..001C");
  i.nome = "Palco 1";
  i.modelo = "iPhone 13";
  CHECAR_IGUAIS(rotuloAparelho(i), "Palco 1 (iPhone 13)");
}

TESTE(converte_avframe_sem_copiar) {
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_YUV420P;
  f->width = 1920;
  f->height = 1080;
  CHECAR(av_frame_get_buffer(f, 32) == 0);
  QuadroSaida q;
  std::string erro;
  CHECAR(converterQuadro(f, q, erro));
  CHECAR(q.formato == QuadroSaida::Formato::I420);
  CHECAR(q.plano[0] == f->data[0] && q.plano[2] == f->data[2]);  // mesmos ponteiros: nada de copia
  CHECAR_IGUAIS(q.passo[1], f->linesize[1]);
  CHECAR(!q.faixaCompleta);
  CHECAR(q.matriz == QuadroSaida::Matriz::BT709);
  f->color_range = AVCOL_RANGE_JPEG;
  f->colorspace = AVCOL_SPC_SMPTE170M;
  CHECAR(converterQuadro(f, q, erro) && q.faixaCompleta && q.matriz == QuadroSaida::Matriz::BT601);
  av_frame_free(&f);

  f = av_frame_alloc();
  f->format = AV_PIX_FMT_NV12;
  f->width = 1280;
  f->height = 720;
  CHECAR(av_frame_get_buffer(f, 32) == 0);
  CHECAR(converterQuadro(f, q, erro) && q.formato == QuadroSaida::Formato::NV12 && q.plano[1] == f->data[1]);
  av_frame_free(&f);

  f = av_frame_alloc();
  f->format = AV_PIX_FMT_YUV444P;
  f->width = 64;
  f->height = 64;
  CHECAR(av_frame_get_buffer(f, 32) == 0);
  CHECAR(!converterQuadro(f, q, erro));
  CHECAR(!erro.empty());
  av_frame_free(&f);
  CHECAR(!converterQuadro(nullptr, q, erro));
}

TESTE(item_que_enche_o_canvas_ganha_bounds_e_pip_nao_e_tocado) {
  TransformacaoItem enche;  // pos 0,0 escala 1, tamanho antigo 1080p no canvas 1080p
  auto a = decidirAjuste(enche, 1920, 1080, 1920, 1080);
  CHECAR(a.aplicar);
  CHECAR(a.posX == 960 && a.posY == 540 && a.boundsLargura == 1920 && a.boundsAltura == 1080);
  // "Ajustar a tela" sem bounds: escala 1.5 sobre 1280x720 tambem enche
  TransformacaoItem escalado;
  escalado.escalaX = escalado.escalaY = 1.5;
  CHECAR(decidirAjuste(escalado, 1280, 720, 1920, 1080).aplicar);
  // picture-in-picture: escala menor e posicao no canto
  TransformacaoItem pip;
  pip.posX = 1400;
  pip.posY = 780;
  pip.escalaX = pip.escalaY = 0.25;
  CHECAR(!decidirAjuste(pip, 1920, 1080, 1920, 1080).aplicar);
  // 720p sem escala no canvas 1080p nao enche: o usuario escolheu assim
  CHECAR(!decidirAjuste(enche, 1280, 720, 1920, 1080).aplicar);
  TransformacaoItem comBounds;
  comBounds.temBounds = true;
  CHECAR(!decidirAjuste(comBounds, 1920, 1080, 1920, 1080).aplicar);
  TransformacaoItem recortado;
  recortado.recortado = true;
  CHECAR(!decidirAjuste(recortado, 1920, 1080, 1920, 1080).aplicar);
  CHECAR(!decidirAjuste(enche, 0, 0, 1920, 1080).aplicar);
  CHECAR(!decidirAjuste(enche, 1920, 1080, 0, 0).aplicar);
}

#ifdef __APPLE__
namespace {
// Anuncio Bonjour falso, so na propria maquina (kDNSServiceInterfaceIndexLocalOnly): nao toca a rede.
DNSServiceRef anunciar(const char* nome, const char* id, uint16_t porta) {
  unsigned char buf[128];
  TXTRecordRef t;
  TXTRecordCreate(&t, sizeof buf, buf);
  TXTRecordSetValue(&t, "id", static_cast<uint8_t>(std::strlen(id)), id);
  TXTRecordSetValue(&t, "v", 1, "1");
  DNSServiceRef r = nullptr;
  DNSServiceRegister(&r, 0, kDNSServiceInterfaceIndexLocalOnly, nome, "_fmcam._tcp", nullptr, nullptr, htons(porta),
                     TXTRecordGetLength(&t), TXTRecordGetBytesPtr(&t), nullptr, nullptr);
  TXTRecordDeallocate(&t);
  return r;
}
}  // namespace

TESTE(wifi_bonjour_lista_depois_do_cabo_sem_duplicar_e_conecta) {
  Cenario c;
  c.ligar(1, 1);  // iPhone 1: no cabo (UDID-1, id uuid-1)
  uint16_t pw = 0;
  auto ipW = fmtest::iphoneFalso(env("FMCAM_IPHONE_FALSO"), env("FMCAM_AMOSTRA"), "Camera Wifi", "iPhone 15", "uuid-wifi-1", pw);
  // anuncia o iPhone so-Wi-Fi e tambem o iPhone 1 (que ja esta no cabo)
  DNSServiceRef a1 = anunciar("Camera Wifi", "uuid-wifi-1", pw);
  DNSServiceRef a2 = anunciar("Camera 1", "uuid-1", c.portas[0]);
  CHECAR(a1 && a2);
  RegistroAparelhos reg(c.sock, criarDescobertaWifi(0xFFFFFFFFu));
  reg.iniciar();
  Contador cabo;
  SessaoFonte sc(cfgPara("UDID-1"), c.sock, &reg);
  sc.definirCallbacks(cabo.cb(), nullptr);
  sc.iniciar();
  CHECAR(esperar([&] { return cabo.quadros > 5; }));
  auto acha = [&](const std::string& udid) {
    for (auto& i : reg.listar()) if (i.udid == udid) return true;
    return false;
  };
  CHECAR(esperar([&] { return acha("wifi:uuid-wifi-1"); }, 10000));
  std::this_thread::sleep_for(500ms);
  CHECAR(!acha("wifi:uuid-1"));  // mesmo id no cabo e no Wi-Fi: so o do cabo
  // So os aparelhos deste teste: um iPhone de verdade com o OBS Cam Mobile aberto na mesma rede
  // tambem aparece no Bonjour e nao pode entrar na conta.
  std::vector<std::decay_t<decltype(reg.listar()[0])>> lista;
  for (auto& i : reg.listar())
    if (i.udid == "UDID-1" || i.udid.rfind("wifi:uuid-", 0) == 0) lista.push_back(i);
  CHECAR_IGUAIS(lista.size(), size_t(2));
  CHECAR(!lista[0].wifi && lista[1].wifi);  // cabo primeiro
  CHECAR_IGUAIS(lista[1].nome, "Camera Wifi");
  CHECAR_IGUAIS(lista[1].ip, "127.0.0.1");

  // fonte escolhida pelo Wi-Fi: normalizar le "wifi:<id>" da lista
  ConfigFonte cw = normalizar(cfgPara("wifi:uuid-wifi-1"));
  CHECAR_IGUAIS(cw.wifiId, "uuid-wifi-1");
  Contador wifi;
  SessaoFonte sw(cw, c.sock, &reg);
  sw.definirCallbacks(wifi.cb(), nullptr);
  sw.iniciar();
  CHECAR(esperar([&] { return wifi.quadros > 20; }, 12000));
  CHECAR(sw.estado().estado == EstadoAparelho::Conectado);
  CHECAR_IGUAIS(sw.estado().nome, "Camera Wifi");
  CHECAR_IGUAIS(wifi.w.load(), 1920);

  // anuncio some: sai da lista
  sw.parar(3000);
  DNSServiceRefDeallocate(a1);
  CHECAR(esperar([&] { return !acha("wifi:uuid-wifi-1"); }, 10000));
  sc.parar(3000);
  DNSServiceRefDeallocate(a2);
  ipW.matar();
  reg.parar();
}
#endif

namespace {
// iPhone de teste que grava os comandos que recebe e manda status com a leitura da camera.
struct FalsoComandos {
  Soquete escuta = kSoqueteInvalido;
  uint16_t porta = 0;
  std::atomic<bool> parar{false};
  std::atomic<int> conexoes{0};
  std::atomic<bool> fecharAgora{false};
  std::atomic<bool> noArRecebido{false};  // espelha o tally recebido, como o app faz no status
  std::mutex m;
  std::vector<std::string> cmds;
  std::vector<std::string> handshakes;  // linhas recebidas, uma por conexao
  std::thread th;
  FalsoComandos() {
    std::string erro;
    escuta = escutarTcp(porta, erro);
    th = std::thread([this] { laco(); });
  }
  ~FalsoComandos() {
    parar = true;
    th.join();
    fecharSoquete(escuta);
  }
  std::vector<std::string> pedidos() {
    std::lock_guard<std::mutex> l(m);
    return handshakes;
  }
  std::vector<std::string> comandos() {
    std::lock_guard<std::mutex> l(m);
    return cmds;
  }
  void laco() {
    while (!parar) {
      Soquete c = aceitar(escuta, 50);
      if (c == kSoqueteInvalido) continue;
      ++conexoes;
      std::string linha;
      if (lerLinha(c, kLinhaHandshakeMaxima, 3000, linha) != ResultadoIO::Ok) {
        fecharSoquete(c);
        continue;
      }
      {
        std::lock_guard<std::mutex> l(m);
        handshakes.push_back(linha);
      }
      RespostaHandshake r;
      r.ok = true;
      r.aparelho = "iPhone 15 Pro Max";
      r.nome = "Falso Tally";
      r.id = "uuid-tally";
      r.w = 1920;  // pedido 2160p30, o iPhone cai para 1080p
      r.h = 1080;
      r.fps = 30;
      r.codec = "h264";
      std::string resp = montarRespostaHandshake(r);
      enviarTudo(c, resp.data(), resp.size());
      ParserPacotes parser;
      uint8_t buf[4096];
      uint64_t proximoStatus = 0;
      while (!parar && !fecharAgora) {
        uint64_t agora = agoraMonotonicoUs();
        if (agora >= proximoStatus) {
          std::string js = "{\"bateria\":0.8,\"carregando\":true,\"temp\":\"nominal\",\"fps\":29.9,\"kbps\":9000,"
                           "\"lente\":\"tele\",\"zoom\":5,\"foco\":\"travado\",\"exposicao\":\"manual\",\"branco\":\"travado\","
                           "\"bias\":-0.7,\"iso\":125,\"obturador\":\"1/60\",\"noar\":" +
                           std::string(noArRecebido ? "true" : "false") + ",\"w\":1920,\"h\":1080,\"fps_alvo\":30}";
          Pacote st;
          st.tipo = kTipoStatus;
          st.tempo = agora;
          st.dados.assign(js.begin(), js.end());
          auto b = serializarPacote(st);
          enviarTudo(c, b.data(), b.size());
          proximoStatus = agora + 200000;
        }
        if (esperarLeitura(c, 50) == 1) {
          long n = lerAlgum(c, buf, sizeof buf);
          if (n <= 0) break;
          parser.alimentar(buf, static_cast<size_t>(n));
          Pacote p;
          while (parser.proximo(p)) {
            if (p.tipo == kTipoComando) {
              std::lock_guard<std::mutex> l(m);
              cmds.emplace_back(p.dados.begin(), p.dados.end());
              if (cmds.back().find("\"valor\":true") != std::string::npos) noArRecebido = true;
              if (cmds.back().find("\"valor\":false") != std::string::npos) noArRecebido = false;
            }
            if (p.tipo == kTipoPing) {
              auto pong = serializarPacote(criarPong(agoraMonotonicoUs(), p.tempo));
              enviarTudo(c, pong.data(), pong.size());
            }
          }
        }
      }
      fecharSoquete(c);
      fecharAgora = false;
    }
  }
};

bool tem(const std::vector<std::string>& v, const std::string& s) {
  for (auto& x : v) if (x == s) return true;
  return false;
}
}  // namespace

TESTE(tally_noar_status_da_camera_e_formato_entregue) {
  FalsoComandos falso;
  RegistroAparelhos reg("/tmp/fmcam_sem_usbmuxd.sock");  // sem observador: so modo Wi-Fi manual
  ConfigFonte cfg;
  cfg.wifi = true;
  cfg.wifiIp = "127.0.0.1";
  cfg.wifiPorta = falso.porta;
  cfg.preset = "2160p30";
  SessaoFonte s(cfg, "/tmp/fmcam_sem_usbmuxd.sock", &reg);
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::Conectado; }));
  auto e = s.estado();
  CHECAR_IGUAIS(e.respLargura, 1920);  // o que o iPhone entregou, nao o que foi pedido
  CHECAR_IGUAIS(e.respAltura, 1080);
  CHECAR(e.respFps == 30);
  PredefinicaoInfo pi;
  CHECAR(infoPredefinicao("2160p30", pi) && pi.largura == 3840 && pi.altura == 2160 && pi.fps == 30);
  CHECAR(infoPredefinicao("1080p120", pi) && pi.largura == 1920 && pi.fps == 120);
  CHECAR(!infoPredefinicao("4320p30", pi));

  // tally
  s.definirNoAr(true);
  CHECAR(esperar([&] { return tem(falso.comandos(), "{\"cmd\":\"noar\",\"valor\":true}"); }));
  s.definirNoAr(false);
  CHECAR(esperar([&] { return tem(falso.comandos(), "{\"cmd\":\"noar\",\"valor\":false}"); }));
  // leitura da camera no registro
  InfoAparelho info;
  CHECAR(esperar([&] { return reg.buscar("wifi:127.0.0.1", info) && info.status.iso == 125; }));
  CHECAR_IGUAIS(info.status.lente, "tele");
  CHECAR(info.status.zoom == 5);
  CHECAR_IGUAIS(info.status.obturador, "1/60");
  CHECAR_IGUAIS(info.status.foco, "travado");
  CHECAR_IGUAIS(info.status.exposicao, "manual");
  CHECAR_IGUAIS(info.status.branco, "travado");
  CHECAR(info.status.temBias && info.status.bias < -0.69 && info.status.bias > -0.71);
  CHECAR(info.status.w == 1920 && info.status.h == 1080 && info.status.fpsAlvo == 30);
  // status de iPhone antigo, sem bitrate nem qualidade_obs: tolera a ausencia
  CHECAR(info.status.bitrate == 0 && !info.status.qualidadeObs && !info.status.presetObs && !info.status.bitrateObs);
  CHECAR(!info.status.noAr);
  s.definirNoAr(true);
  CHECAR(esperar([&] { return reg.buscar("wifi:127.0.0.1", info) && info.status.noAr; }));
  CHECAR(esperar([&] { return reg.buscar("wifi:127.0.0.1", info) && info.status.noArIphone; }));  // o iPhone confirmou no status

  // reconectou: a sessao repete o tally (o iPhone nasce fora do ar a cada conexao)
  int antes = static_cast<int>(falso.comandos().size());
  falso.fecharAgora = true;
  CHECAR(esperar([&] { return falso.conexoes >= 2; }, 12000));
  CHECAR(esperar([&] { return static_cast<int>(falso.comandos().size()) > antes; }));
  CHECAR_IGUAIS(falso.comandos().back(), "{\"cmd\":\"noar\",\"valor\":true}");
  s.parar(3000);
}

TESTE(lente_so_vai_no_handshake_se_o_operador_escolheu) {
  ConfigFonte padrao;
  CHECAR(padrao.lente.empty());  // padrao: manter a lente do iPhone
  CHECAR(lenteValida(""));
  CHECAR(normalizar(padrao).lente.empty());
  CHECAR(normalizar(padrao).preset.empty());  // predefinicao e bitrate tambem: o padrao e a qualidade do iPhone
  CHECAR_IGUAIS(normalizar(padrao).bitrateKbps, 0);
  FalsoComandos falso;
  RegistroAparelhos reg("/tmp/fmcam_sem_usbmuxd.sock");
  auto sobe = [&](const std::string& lente) {
    ConfigFonte cfg;
    cfg.wifi = true;
    cfg.wifiIp = "127.0.0.1";
    cfg.wifiPorta = falso.porta;
    cfg.lente = lente;
    return std::make_unique<SessaoFonte>(cfg, "/tmp/fmcam_sem_usbmuxd.sock", &reg);
  };
  {
    auto s = sobe("");
    s->iniciar();
    CHECAR(esperar([&] { return falso.pedidos().size() >= 1; }));
    auto h = falso.pedidos()[0];
    CHECAR(h.find("\"preset\"") == std::string::npos);  // padrao: a qualidade e a do iPhone
    // (a ausencia do campo "lente" no handshake e o teste core_omite_lente_vazia, abaixo)
    s->parar(3000);
  }
  size_t antes = falso.pedidos().size();
  {
    auto s = sobe("tele");
    s->iniciar();
    CHECAR(esperar([&] { return falso.pedidos().size() > antes; }));
    CHECAR(falso.pedidos().back().find("\"lente\":\"tele\"") != std::string::npos);
    s->parar(3000);
  }
  // trocar a lente nas propriedades muda a conexao (reconecta com a lente no handshake)
  ConfigFonte a, b;
  b.lente = "ultra";
  CHECAR(mudouConexao(a, b));
}

// DEPENDE DE UM AJUSTE NO core/protocolo.cc: montarPedidoHandshake hoje escreve "lente" sempre, mesmo vazia
// (vira "lente":"", que o iPhone trata como valor invalido e cai em "wide"). Precisa de:
//   if (!p.lente.empty()) s += ",\"lente\":\"" + escaparJson(p.lente) + "\"";
TESTE(core_omite_lente_vazia_no_handshake) {
  fmappcam::PedidoHandshake p;
  CHECAR(p.lente == "wide");  // padrao do core; a extensao zera
  p.lente.clear();
  std::string linha = fmappcam::montarPedidoHandshake(p);
  CHECAR(linha.find("\"lente\"") == std::string::npos);
  p.lente = "tele";
  CHECAR(fmappcam::montarPedidoHandshake(p).find("\"lente\":\"tele\"") != std::string::npos);
}

TESTE(handshake_sem_preset_nem_bitrate_quando_do_iphone_e_com_eles_quando_fixado) {
  FalsoComandos falso;
  RegistroAparelhos reg("/tmp/fmcam_sem_usbmuxd.sock");
  auto sobe = [&](const std::string& preset, int kbps) {
    ConfigFonte cfg;
    cfg.wifi = true;
    cfg.wifiIp = "127.0.0.1";
    cfg.wifiPorta = falso.porta;
    cfg.preset = preset;
    cfg.bitrateKbps = kbps;
    return std::make_unique<SessaoFonte>(cfg, "/tmp/fmcam_sem_usbmuxd.sock", &reg);
  };
  {  // tudo "do iPhone" (o padrao)
    auto s = sobe("", 0);
    s->iniciar();
    CHECAR(esperar([&] { return falso.pedidos().size() >= 1; }));
    auto h = falso.pedidos()[0];
    CHECAR(h.find("preset") == std::string::npos);
    CHECAR(h.find("bitrate") == std::string::npos);
    s->parar(3000);
  }
  size_t antes = falso.pedidos().size();
  {  // so o preset fixado
    auto s = sobe("720p60", 0);
    s->iniciar();
    CHECAR(esperar([&] { return falso.pedidos().size() > antes; }));
    auto h = falso.pedidos().back();
    CHECAR(h.find("\"preset\":\"720p60\"") != std::string::npos);
    CHECAR(h.find("bitrate") == std::string::npos);
    s->parar(3000);
  }
  antes = falso.pedidos().size();
  {  // preset e bitrate fixados
    auto s = sobe("1080p60", 6000);
    s->iniciar();
    CHECAR(esperar([&] { return falso.pedidos().size() > antes; }));
    auto h = falso.pedidos().back();
    CHECAR(h.find("\"preset\":\"1080p60\"") != std::string::npos);
    CHECAR(h.find("\"bitrate\":6000000") != std::string::npos);
    s->parar(3000);
  }
  // mudar so o preset ou so o bitrate nas propriedades reconecta
  ConfigFonte a, b = a;
  CHECAR(!mudouConexao(a, b));
  b.preset = "720p30";
  CHECAR(mudouConexao(a, b));
  b = a;
  b.bitrateKbps = 6000;
  CHECAR(mudouConexao(a, b));
}

TESTE(migracao_padrao_antigo_vira_do_iphone_e_o_resto_e_respeitado) {
  // salvo igual ao padrao antigo: nao foi escolha, vira "do iPhone"
  ValoresSalvos v;
  v.temPreset = v.temBitrate = true;
  v.preset = "1080p30";
  v.bitrateKbps = 10000;
  auto r = migrarValores(v);
  CHECAR(r.preset && r.bitrate && !r.lente && r.alterou());
  CHECAR(v.preset.empty() && v.bitrateKbps == 0);
  CHECAR_IGUAIS(v.versao, kVersaoConfig);
  // qualquer outro valor salvo e escolha do operador
  ValoresSalvos w;
  w.temPreset = w.temBitrate = true;
  w.preset = "720p60";
  w.bitrateKbps = 6000;
  CHECAR(!migrarValores(w).alterou());
  CHECAR_IGUAIS(w.preset, "720p60");
  CHECAR_IGUAIS(w.bitrateKbps, 6000);
  // um dos dois igual ao antigo: so ele migra
  ValoresSalvos x;
  x.temPreset = x.temBitrate = true;
  x.preset = "1080p60";
  x.bitrateKbps = 10000;
  r = migrarValores(x);
  CHECAR(!r.preset && r.bitrate);
  CHECAR_IGUAIS(x.preset, "1080p60");
  CHECAR_IGUAIS(x.bitrateKbps, 0);
  // sem valor salvo (so o padrao do OBS) nada muda
  ValoresSalvos y;
  y.preset = "1080p30";
  y.bitrateKbps = 10000;
  CHECAR(!migrarValores(y).alterou());
  // a migracao da lente (versao 2) continua valendo
  ValoresSalvos z;
  z.temLente = true;
  z.lente = "wide";
  CHECAR(migrarValores(z).lente && z.lente.empty());
  // ja migrada: roda uma vez so (quem escolheu 1080p30 depois da migracao continua com 1080p30)
  ValoresSalvos j;
  j.versao = kVersaoConfig;
  j.temPreset = j.temBitrate = true;
  j.preset = "1080p30";
  j.bitrateKbps = 10000;
  CHECAR(!migrarValores(j).alterou());
  CHECAR_IGUAIS(j.preset, "1080p30");
  CHECAR_IGUAIS(j.bitrateKbps, 10000);
  // versao 2 (so a lente foi migrada antes): o preset e o bitrate antigos ainda migram
  ValoresSalvos k;
  k.versao = 2;
  k.temPreset = k.temBitrate = true;
  k.preset = "1080p30";
  k.bitrateKbps = 10000;
  CHECAR(migrarValores(k).alterou() && k.preset.empty() && k.bitrateKbps == 0);
  // normalizar: 0 e do iPhone, valor acima da faixa vai para o limite
  ConfigFonte c;
  c.bitrateKbps = 0;
  CHECAR_IGUAIS(normalizar(c).bitrateKbps, 0);
  c.bitrateKbps = -5;
  CHECAR_IGUAIS(normalizar(c).bitrateKbps, 0);
  c.bitrateKbps = 500;
  CHECAR_IGUAIS(normalizar(c).bitrateKbps, 1000);
}

namespace {
// Roda um iphone-falso real (Wi-Fi manual, sem usbmuxd) e devolve a sessao ja iniciada.
struct SessaoFalso {
  fmtest::Proc proc;
  uint16_t porta = 0;
  RegistroAparelhos reg{"/tmp/fmcam_sem_usbmuxd.sock"};
  Contador cont;
  std::unique_ptr<SessaoFonte> s;
  SessaoFalso(const std::vector<std::string>& extras, ConfigFonte cfg) {
    proc = fmtest::iphoneFalso(env("FMCAM_IPHONE_FALSO"), env("FMCAM_AMOSTRA"), "Camera Q", "iPhone 17 Pro", "uuid-q", porta, extras);
    cfg.wifi = true;
    cfg.wifiIp = "127.0.0.1";
    cfg.wifiPorta = porta;
    s = std::make_unique<SessaoFonte>(cfg, "/tmp/fmcam_sem_usbmuxd.sock", &reg);
    s->definirCallbacks(cont.cb(), nullptr);
    s->iniciar();
  }
  ~SessaoFalso() {
    s->parar(3000);
    proc.matar();
  }
  bool status(InfoAparelho& i) { return reg.buscar("wifi:127.0.0.1", i); }
};
}  // namespace

TESTE(qualidade_do_iphone_status_reflete_e_troca_no_meio_da_conexao_sem_reconectar) {
  // OBS "do iPhone": o iPhone usa a qualidade propria (1080p60, 12 Mbps) e diz que nao e do OBS. Depois de 3 s
  // ele troca sozinho para 720p30 a 6 Mbps, sem derrubar a conexao.
  SessaoFalso f({"--preset-proprio", "1080p60", "--bitrate-proprio", "12000000", "--trocar-preset-apos-s", "3", "--para",
                 "720p30", "--para-bitrate", "6000000", "--arquivo-troca", env("FMCAM_AMOSTRA720")},
                ConfigFonte{});
  InfoAparelho info;
  CHECAR(esperar([&] { return f.cont.quadros > 10 && f.status(info) && info.status.bitrate > 0; }, 8000));
  CHECAR_IGUAIS(info.status.bitrate, int64_t(12000000));
  CHECAR(!info.status.qualidadeObs && !info.status.presetObs && !info.status.bitrateObs);
  CHECAR(info.status.w == 1920 && info.status.h == 1080 && info.status.fpsAlvo == 60);
  CHECAR_IGUAIS(f.cont.w.load(), 1920);
  CHECAR_IGUAIS(f.s->estado().respFps, 60.0);

  // a troca chega: formato novo no video e no status
  CHECAR(esperar([&] { return f.cont.w == 1280 && f.cont.h == 720; }, 12000));
  CHECAR(esperar([&] { return f.status(info) && info.status.bitrate == 6000000 && info.status.h == 720; }, 5000));
  CHECAR(info.status.w == 1280 && info.status.fpsAlvo == 30);
  CHECAR(!info.status.qualidadeObs);
  auto e = f.s->estado();
  CHECAR(e.estado == EstadoAparelho::Conectado);
  CHECAR_IGUAIS(e.reconexoes, 0);  // a mesma conexao
  CHECAR_IGUAIS(e.respLargura, 1280);
  CHECAR_IGUAIS(e.respAltura, 720);
  CHECAR_IGUAIS(e.respFps, 30.0);
  CHECAR_IGUAIS(e.largura, 1280);
  CHECAR_IGUAIS(e.altura, 720);
  int antes = f.cont.quadros;
  CHECAR(esperar([&] { return f.cont.quadros > antes + 20; }));  // segue entregando depois da troca
  CHECAR_IGUAIS(f.s->estado().errosDecodificacao, uint64_t(0));
  CHECAR_IGUAIS(f.s->estado().reconexoes, 0);
}

TESTE(qualidade_fixada_no_obs_chega_no_status_como_qualidade_obs) {
  ConfigFonte cfg;
  cfg.preset = "720p60";
  cfg.bitrateKbps = 6000;
  SessaoFalso f({"--preset-proprio", "1080p30"}, cfg);  // o proprio do iPhone nao vale: o OBS fixou
  InfoAparelho info;
  CHECAR(esperar([&] { return f.cont.quadros > 10 && f.status(info) && info.status.bitrate > 0; }, 8000));
  CHECAR_IGUAIS(info.status.bitrate, int64_t(6000000));
  CHECAR(info.status.qualidadeObs && info.status.presetObs && info.status.bitrateObs);
}

TESTE(trava_por_campo_so_o_bitrate_fixado_marca_so_o_bitrate) {
  // caso real do dono: predefinicao "do iPhone" e bitrate 20000 salvo
  ConfigFonte cfg;
  cfg.bitrateKbps = 20000;
  SessaoFalso f({"--preset-proprio", "1080p60"}, cfg);
  InfoAparelho info;
  CHECAR(esperar([&] { return f.cont.quadros > 10 && f.status(info) && info.status.bitrate > 0; }, 8000));
  CHECAR_IGUAIS(info.status.bitrate, int64_t(20000000));
  CHECAR(info.status.qualidadeObs && info.status.bitrateObs && !info.status.presetObs);
  CHECAR(info.status.w == 1920 && info.status.fpsAlvo == 60);  // o formato segue sendo o do iPhone
}

TESTE(modelo_comercial_do_iphone_tabela_desconhecido_e_nome_pronto) {
  // fonte da tabela: gist adamawolf/3048717 (Apple_mobile_device_types), conferida em 18/11/2026
  CHECAR_IGUAIS(nomeComercialIphone("iPhone12,1"), "iPhone 11");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone13,2"), "iPhone 12");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone14,5"), "iPhone 13");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone15,2"), "iPhone 14 Pro");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone16,2"), "iPhone 15 Pro Max");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone17,3"), "iPhone 16");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone18,1"), "iPhone 17 Pro");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone18,2"), "iPhone 17 Pro Max");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone18,3"), "iPhone 17");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone18,4"), "iPhone Air");
  // identificador que nao esta na tabela nunca e inventado
  CHECAR_IGUAIS(nomeComercialIphone("iPhone99,9"), "iPhone (iPhone99,9)");
  CHECAR_IGUAIS(nomeComercialIphone("iPhone11,8"), "iPhone (iPhone11,8)");  // anterior a tabela
  // nome ja pronto (iphone-falso) passa como esta, e vazio fica vazio
  CHECAR_IGUAIS(nomeComercialIphone("iPhone 13"), "iPhone 13");
  CHECAR_IGUAIS(nomeComercialIphone(""), "");
}

TESTE(rotulos_de_iphones_do_mesmo_modelo_ganham_numero_estavel_por_udid) {
  std::vector<IphoneParaRotulo> v = {{"UDID-C", "iPhone 15 Pro Max"}, {"UDID-A", "iPhone 15 Pro Max"},
                                     {"UDID-B", "iPhone 13"},         {"UDID-D", "iPhone 15 Pro Max"},
                                     {"wifi:X", ""}};
  auto r = rotulosIphones(v);
  CHECAR_IGUAIS(r["UDID-A"], "iPhone 15 Pro Max");      // o menor UDID fica com o nome puro
  CHECAR_IGUAIS(r["UDID-C"], "iPhone 15 Pro Max (2)");
  CHECAR_IGUAIS(r["UDID-D"], "iPhone 15 Pro Max (3)");
  CHECAR_IGUAIS(r["UDID-B"], "iPhone 13");
  CHECAR_IGUAIS(r["wifi:X"], "iPhone");                 // modelo ainda desconhecido
  // estavel: a ordem de entrada nao muda o resultado
  std::vector<IphoneParaRotulo> inv(v.rbegin(), v.rend());
  CHECAR(rotulosIphones(inv) == r);
}

TESTE(rotulo_do_registro_usa_modelo_comercial_e_o_mesmo_rotulo_serve_para_cabo_e_wifi) {
  Cenario c;
  c.ligar(1, 1);
  c.ligar(2, 2);
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  CHECAR(esperar([&] { return reg.listar().size() == 2; }));
  SessaoFonte s1(cfgPara("UDID-1"), c.sock, &reg), s2(cfgPara("UDID-2"), c.sock, &reg);
  Contador k1, k2;
  s1.definirCallbacks(k1.cb(), nullptr);
  s2.definirCallbacks(k2.cb(), nullptr);
  s1.iniciar();
  s2.iniciar();
  CHECAR(esperar([&] { return k1.quadros > 5 && k2.quadros > 5; }));
  // os iphone-falso mandam "iPhone 11" e "iPhone 12" como modelo (nome pronto)
  CHECAR_IGUAIS(reg.rotulo("UDID-1"), "iPhone 11");
  CHECAR_IGUAIS(reg.rotulo("UDID-2"), "iPhone 12");
  CHECAR_IGUAIS(reg.rotulo("UDID-NAO-EXISTE"), "");
  s1.parar(3000);
  s2.parar(3000);
  reg.parar();
}

namespace {
bool registrouSemCabo(RegistroAparelhos& reg, const std::string& udid) {
  InfoAparelho i;
  return reg.buscar(udid, i) && i.fontes > 0 && i.estado == EstadoAparelho::SemCabo;
}
}  // namespace

TESTE(macos_apaga_a_entrada_usb_a_fonte_segue_pela_rede_e_volta_ao_cabo) {
  Cenario c;
  c.ligar(1, 1);  // entrada USB id 1
  c.fake.anexar({11, "UDID-1", "Network"}, "127.0.0.1", c.portas[0]);  // a mesma iPhone pela rede (Wi-Fi sync)
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  Contador cont;
  std::atomic<bool> viuSemCabo{false};
  SessaoFonte s(cfgPara("UDID-1"), c.sock, &reg);
  s.definirCallbacks(cont.cb(), [&](const EstadoSessao& e) {
    if (e.estado == EstadoAparelho::SemCabo) viuSemCabo = true;
  });
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::Conectado && cont.quadros > 30; }));
  CHECAR(!s.estado().pelaRede);  // com a entrada USB ela tem prioridade
  CHECAR_IGUAIS(s.estado().reconexoes, 0);

  // 1) o macOS apaga a entrada USB (o cabo continua ligado): a fonte recebe quadros pela entrada de rede
  int antes = cont.quadros;
  c.fake.desanexar(1);
  CHECAR(esperar([&] {
    if (registrouSemCabo(reg, "UDID-1")) viuSemCabo = true;
    auto e = s.estado();
    return e.estado == EstadoAparelho::Conectado && e.pelaRede && cont.quadros > antes + 30;
  }, 12000));
  CHECAR(!viuSemCabo);  // nunca "fora do cabo", nem na sessao nem no registro
  CHECAR_IGUAIS(s.estado().mensagem, "conectado pelo Wi-Fi (cabo indisponivel)");
  InfoAparelho info;
  CHECAR(reg.buscar("UDID-1", info) && info.pelaRede && info.estado == EstadoAparelho::Conectado);
  int reconPelaRede = s.estado().reconexoes;
  CHECAR(reconPelaRede >= 1);

  // 2) a entrada USB volta (evento Attached): migra para o cabo, e so entao solta a rede
  antes = cont.quadros;
  c.fake.anexar({12, "UDID-1", "USB"}, "127.0.0.1", c.portas[0]);
  CHECAR(esperar([&] {
    auto e = s.estado();
    return e.estado == EstadoAparelho::Conectado && !e.pelaRede && e.reconexoes > reconPelaRede && cont.quadros > antes + 30;
  }, 12000));
  CHECAR_IGUAIS(s.estado().mensagem, "conectado");
  CHECAR_IGUAIS(s.estado().errosDecodificacao, uint64_t(0));
  CHECAR(!viuSemCabo);
  s.parar(3000);
  reg.parar();
}

TESTE(sem_nenhuma_entrada_do_udid_continua_fora_do_cabo) {
  Cenario c;
  c.fake.anexar({5, "OUTRO-UDID", "Network"}, "127.0.0.1", 1);  // rede de outro iPhone nao serve
  RegistroAparelhos reg(c.sock);
  reg.iniciar();
  SessaoFonte s(cfgPara("UDID-X"), c.sock, &reg);
  s.iniciar();
  CHECAR(esperar([&] { return s.estado().estado == EstadoAparelho::SemCabo; }, 6000));
  CHECAR(!s.estado().pelaRede);
  s.parar(3000);
  reg.parar();
}
