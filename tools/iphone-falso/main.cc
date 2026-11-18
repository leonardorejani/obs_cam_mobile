// iphone-falso: fala o lado iPhone do protocolo OBS Cam Mobile v1 e manda um H.264 gravado em laco.
// Serve para testar o core, o visor e a extensao sem aparelho.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include "annexb.h"
#include "protocolo.h"
#include "rede.h"

using namespace fmappcam;

namespace {

struct Opcoes {
  uint16_t porta = kPortaPadrao;
  std::string arquivo;
  double fps = 30;
  int largura = 1920, altura = 1080;
  int64_t desvioUs = 0;
  int perderQuadro = -1;  // indice (0 = primeiro) do quadro que nao e enviado, uma vez por conexao
  int atrasoMs = 0;
  // Simula o caso real do iPhone (18/11/2026): a conexao segue aberta mas os quadros param.
  // Uma vez no processo: depois de travarAposS segundos de conexao, fica travarPorS sem mandar nada.
  double travarAposS = 0, travarPorS = 5;
  // Qualidade "salva no iPhone": vale quando o handshake vem sem preset/bitrate (docs/PROTOCOLO.md, 1). Sem
  // --preset-proprio o formato e o do arquivo (--largura, --altura, --fps).
  std::string presetProprio;
  int64_t bitrateProprio = 10000000;
  // Troca de qualidade feita no iPhone no meio da conexao: depois de trocarAposS segundos, passa para `trocarPara`
  // (e `trocarBitrate`, se dado) sem derrubar a conexao: tipo 1 novo, quadro-chave e status novo. Com arquivoTroca,
  // o video passa a sair desse arquivo (outra resolucao); sem ele so o formato declarado muda.
  double trocarAposS = 0;
  std::string trocarPara, arquivoTroca;
  int64_t trocarBitrate = 0;
  std::string aparelho = "iPhone Falso";
  std::string nome, id;
  bool silencioso = false;
};

Opcoes g;
FluxoH264 g_fluxo;
FluxoH264 g_fluxoTroca;

struct Formato {
  int w = 0, h = 0;
  double fps = 0;
};
// "720p30" -> 1280x720 a 30 (16:9, como a extensao)
bool lerPreset(const std::string& p, Formato& f) {
  size_t i = p.find('p');
  if (i == std::string::npos || i == 0 || i + 1 >= p.size()) return false;
  int alt = std::atoi(p.substr(0, i).c_str());
  double fps = std::atof(p.substr(i + 1).c_str());
  if (alt <= 0 || fps <= 0) return false;
  f.h = alt;
  f.w = alt * 16 / 9;
  f.fps = fps;
  return true;
}

// Atende um cliente por vez. Quem chega com a vaga tomada espera um instante: se o dono ja fechou do lado dele,
// a sessao dele e acordada na hora (nao espera o poll) e a vaga passa para o novo. Dono vivo = "ocupado".
struct Vaga {
  std::mutex m;
  std::condition_variable cv;
  bool ocupada = false;
  Soquete dono = kSoqueteInvalido;

  bool tomar(Soquete s, int graceMs) {
    std::unique_lock<std::mutex> l(m);
    const auto fim = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    for (;;) {
      if (!ocupada) {
        ocupada = true;
        dono = s;
        return true;
      }
      if (parFechou(dono)) desligarSoquete(dono);
      if (std::chrono::steady_clock::now() >= fim) return false;
      cv.wait_for(l, std::chrono::milliseconds(2));
    }
  }
  void liberar() {
    {
      std::lock_guard<std::mutex> l(m);
      ocupada = false;
      dono = kSoqueteInvalido;
    }
    cv.notify_all();
  }
};
Vaga g_vaga;
constexpr int kGraceVagaMs = 200;
std::atomic<bool> g_jaTravou{false};

uint64_t tempoIphone(uint64_t monoUs) { return static_cast<uint64_t>(static_cast<int64_t>(monoUs) + g.desvioUs); }

void log(const std::string& m) {
  if (!g.silencioso) std::fprintf(stderr, "[iphone-falso:%u] %s\n", unsigned(g.porta), m.c_str());
}

void enviarResposta(Soquete s, const RespostaHandshake& r) {
  std::string l = montarRespostaHandshake(r);
  enviarTudo(s, l.data(), l.size());
}

void falhaHandshake(Soquete s, const std::string& erro) {
  RespostaHandshake r;
  r.erro = erro;
  if (erro == "versao") r.vSuportadas = {1};
  enviarResposta(s, r);
  // espera o cliente fechar (evita RST que apaga a resposta no lado dele)
  uint8_t b[256];
  for (int i = 0; i < 20 && esperarLeitura(s, 100) == 1; ++i)
    if (lerAlgum(s, b, sizeof b) <= 0) break;
}

struct Pendente {
  uint64_t liberarEm;
  std::vector<uint8_t> bytes;
  bool chave;
};

constexpr size_t kFilaMinimaBytes = 1u << 20;   // docs/PROTOCOLO.md, 4: no minimo 1 MiB
constexpr size_t kFilaTetoBytes = 8u << 20;     // acima disso a conexao e fechada
constexpr uint64_t kIntervaloPedidoChaveUs = 1000000;

// Fila de envio com a politica da secao 4 do protocolo: nunca acumula video velho.
class FilaEnvio {
 public:
  explicit FilaEnvio(int64_t bitrate)
      : limite_(std::max<size_t>(kFilaMinimaBytes, static_cast<size_t>(bitrate / 8))) {}

  bool vazia() const { return itens_.empty(); }
  size_t bytes() const { return bytes_; }
  size_t quadrosDescartados() const { return descartados_; }
  size_t limite() const { return limite_; }

  // Controle (config, status, pong): nunca descartado.
  void controle(std::vector<uint8_t> b) { empurrar(std::move(b), false, false); }

  // Quadro de video. Devolve true se entrou na fila. `pedirChave` e ligado quando o descarte pede um IDR novo.
  bool quadro(std::vector<uint8_t> b, bool chave, bool& pedirChave) {
    pedirChave = false;
    if (chave) {
      if (bytes_ + b.size() > limite_) purgarVideoNaoIniciado();  // o quadro-chave mais novo vai inteiro
      descartando_ = false;
    } else {
      if (descartando_) {
        ++descartados_;
        return false;
      }
      if (bytes_ + b.size() > limite_) {
        descartando_ = true;  // sem referencia nao adianta mandar os proximos: espera o IDR
        pedirChave = true;
        ++descartados_;
        return false;
      }
    }
    empurrar(std::move(b), true, chave);
    return true;
  }

  bool estourouTeto() const { return bytes_ > kFilaTetoBytes; }

  // Manda o que o soquete aceitar. false = erro de envio.
  bool drenar(Soquete s, uint64_t& enviados) {
    while (!itens_.empty()) {
      Item& f = itens_.front();
      long n = enviarAlgum(s, f.bytes.data() + off_, f.bytes.size() - off_);
      if (n < 0) return false;
      if (n == 0) return true;
      off_ += static_cast<size_t>(n);
      bytes_ -= static_cast<size_t>(n);
      enviados += static_cast<uint64_t>(n);
      if (off_ == f.bytes.size()) {
        itens_.pop_front();
        off_ = 0;
      }
    }
    return true;
  }

 private:
  struct Item {
    std::vector<uint8_t> bytes;
    bool video;
    bool chave;
  };
  void empurrar(std::vector<uint8_t> b, bool video, bool chave) {
    bytes_ += b.size();
    itens_.push_back({std::move(b), video, chave});
  }
  // Tira da fila todo quadro de video que ainda nao comecou a sair (o da frente, se ja saiu em parte, termina).
  void purgarVideoNaoIniciado() {
    for (size_t i = itens_.size(); i-- > 0;) {
      if (i == 0 && off_ > 0) continue;
      if (!itens_[i].video) continue;
      bytes_ -= itens_[i].bytes.size();
      ++descartados_;
      itens_.erase(itens_.begin() + static_cast<long>(i));
    }
  }

  size_t limite_;
  std::deque<Item> itens_;
  size_t off_ = 0;
  size_t bytes_ = 0;
  bool descartando_ = false;
  size_t descartados_ = 0;
};

void sessao(Soquete s) {
  ajustarTimeoutEnvio(s, 5000);
  std::string linha;
  ResultadoIO r = lerLinha(s, kLinhaHandshakeMaxima, 3000, linha);
  if (r == ResultadoIO::Fechado) return;
  if (r != ResultadoIO::Ok) {
    log("handshake invalido ou sem fim em 3 s");
    falhaHandshake(s, "handshake");
    return;
  }
  PedidoHandshake ped;
  ErroHandshake e = lerPedidoHandshake(linha, ped);
  if (e == ErroHandshake::Handshake) { falhaHandshake(s, "handshake"); return; }
  if (e == ErroHandshake::Versao) { falhaHandshake(s, "versao"); return; }
  if (!g_vaga.tomar(s, kGraceVagaMs)) {
    log("recusado: ocupado");
    falhaHandshake(s, "ocupado");
    return;
  }
  struct Libera { ~Libera() { g_vaga.liberar(); } } libera;

  // Preset e bitrate ausentes = o iPhone usa o que tem salvo. Presentes = vale o do cliente nesta conexao.
  const bool qualidadeObs = !ped.preset.empty() || ped.bitrate > 0;
  Formato fmt{g.largura, g.altura, g.fps};
  if (ped.preset.empty() && !g.presetProprio.empty()) lerPreset(g.presetProprio, fmt);
  int64_t bitrate = ped.bitrate > 0 ? ped.bitrate : g.bitrateProprio;

  RespostaHandshake ok;
  ok.ok = true;
  ok.aparelho = g.aparelho;
  ok.nome = g.nome;
  ok.id = g.id;
  ok.w = fmt.w;
  ok.h = fmt.h;
  ok.fps = fmt.fps;
  enviarResposta(s, ok);
  log("cliente conectado: " + ped.cliente + " preset=" + (ped.preset.empty() ? "(do iPhone)" : ped.preset) +
      " bitrate=" + (ped.bitrate > 0 ? std::to_string(ped.bitrate) : "(do iPhone)"));

  // Dai em diante o envio nunca bloqueia: o que o cliente nao le fica na fila, com a politica da secao 4.
  tornarNaoBloqueante(s);
  FilaEnvio saida(bitrate);
  uint64_t intervalo = static_cast<uint64_t>(1e6 / fmt.fps);
  const uint64_t t0 = agoraMonotonicoUs();
  uint64_t tBase = t0;  // a grade de quadros (tBase + k * intervalo) recomeca quando a qualidade muda
  const FluxoH264* fl = &g_fluxo;
  bool trocou = false;
  saida.controle(serializarPacote(criarConfig(tempoIphone(t0), fl->config)));
  uint64_t k = 0;      // quadros gerados nesta conexao
  size_t idx = 0;      // posicao no arquivo
  uint64_t ultimoRx = t0, proxStatus = t0 + 2000000, bytesJanela = 0, quadrosJanela = 0, inicioJanela = t0;
  uint64_t ultimoPedidoChave = 0;
  std::deque<Pendente> fila;
  ParserPacotes parser;
  std::vector<uint8_t> buf(65536);
  uint64_t travadoAte = 0;

  auto irParaChave = [&](const char* motivo) {
    size_t salto = 0;
    while (salto < fl->quadros.size() && !fl->quadros[idx].chave) {
      idx = (idx + 1) % fl->quadros.size();
      ++salto;
    }
    log(std::string("keyframe (") + motivo + "), indo ao IDR " + std::to_string(idx) + " (pulou " + std::to_string(salto) + ")");
  };

  for (;;) {
    uint64_t agora = agoraMonotonicoUs();
    // gera os quadros que ja venceram
    if (g.travarAposS > 0 && agora >= t0 + static_cast<uint64_t>(g.travarAposS * 1e6) && !g_jaTravou.exchange(true)) {
      log("travando o envio de quadros por " + std::to_string(g.travarPorS) + " s");
      travadoAte = agora + static_cast<uint64_t>(g.travarPorS * 1e6);
    }
    if (agora < travadoAte) {  // trava tudo: nem quadro, nem status, nem pong (como a fila do app parada)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    while (tBase + k * intervalo <= agora) {
      uint64_t sched = tBase + k * intervalo;
      const UnidadeAcesso& au = fl->quadros[idx];
      idx = (idx + 1) % fl->quadros.size();
      bool perde = (g.perderQuadro >= 0 && k == static_cast<uint64_t>(g.perderQuadro));
      if (!perde) {
        fila.push_back({sched + static_cast<uint64_t>(g.atrasoMs) * 1000,
                        serializarPacote(criarQuadro(tempoIphone(sched), au.chave, au.dados.data(), au.dados.size())),
                        au.chave});
      } else {
        log("quadro " + std::to_string(k) + " descartado de proposito");
      }
      ++k;
    }
    while (!fila.empty() && fila.front().liberarEm <= agora) {
      bool pedirChave = false;
      if (saida.quadro(std::move(fila.front().bytes), fila.front().chave, pedirChave)) ++quadrosJanela;
      fila.pop_front();
      if (pedirChave && agora - ultimoPedidoChave >= kIntervaloPedidoChaveUs) {
        ultimoPedidoChave = agora;
        irParaChave("fila cheia");
      }
    }
    if (!trocou && !qualidadeObs && g.trocarAposS > 0 &&  // OBS fixou: o iPhone nao troca
        agora >= t0 + static_cast<uint64_t>(g.trocarAposS * 1e6)) {
      trocou = true;
      Formato novo;
      if (lerPreset(g.trocarPara, novo)) {
        fmt = novo;
        if (g.trocarBitrate > 0) bitrate = g.trocarBitrate;
        if (!g_fluxoTroca.quadros.empty()) fl = &g_fluxoTroca;
        idx = 0;  // o primeiro quadro do arquivo e sempre IDR (checado ao carregar)
        fila.clear();
        intervalo = static_cast<uint64_t>(1e6 / fmt.fps);
        tBase = agora;
        k = 0;
        // config novo, e o proximo quadro e quadro-chave; o status novo sai no ciclo normal (2 s), sem status imediato
        saida.controle(serializarPacote(criarConfig(tempoIphone(agora), fl->config)));
        log("qualidade trocada no iPhone para " + g.trocarPara);
      } else {
        log("--para invalido: " + g.trocarPara);
      }
    }
    if (agora >= proxStatus) {
      double seg = (agora - inicioJanela) / 1e6;
      char js[400];
      std::snprintf(js, sizeof js,
                    "{\"bateria\":0.82,\"carregando\":true,\"temp\":\"nominal\",\"fps\":%.1f,\"kbps\":%d,\"w\":%d,\"h\":%d,"
                    "\"fps_alvo\":%g,\"bitrate\":%lld,\"qualidade_obs\":%s,\"preset_obs\":%s,\"bitrate_obs\":%s}",
                    seg > 0 ? quadrosJanela / seg : 0.0, seg > 0 ? int(bytesJanela * 8 / 1000 / seg) : 0, fmt.w, fmt.h, fmt.fps,
                    static_cast<long long>(bitrate), qualidadeObs ? "true" : "false",
                    !ped.preset.empty() ? "true" : "false", ped.bitrate > 0 ? "true" : "false");
      saida.controle(serializarPacote(Pacote{kTipoStatus, tempoIphone(agora), std::vector<uint8_t>(js, js + std::strlen(js))}));
      proxStatus = agora + 2000000;
      bytesJanela = quadrosJanela = 0;
      inicioJanela = agora;
    }
    if (saida.estourouTeto()) { log("fila de envio passou de 8 MiB, derrubando"); return; }
    if (!saida.drenar(s, bytesJanela)) { log("envio falhou"); return; }
    if (agora - ultimoRx > 5000000) { log("5 s sem dados do cliente, derrubando"); return; }

    uint64_t prox = tBase + k * intervalo;
    if (!fila.empty() && fila.front().liberarEm < prox) prox = fila.front().liberarEm;
    if (proxStatus < prox) prox = proxStatus;
    agora = agoraMonotonicoUs();
    int espera = prox > agora ? static_cast<int>((prox - agora + 999) / 1000) : 0;
    if (espera > 50) espera = 50;
    bool legivel, gravavel;
    int pr = esperarES(s, !saida.vazia(), espera, legivel, gravavel);
    if (pr < 0) return;
    if (pr == 0 || !legivel) continue;
    long n = lerAlgum(s, buf.data(), buf.size());
    if (n <= 0) { log("cliente saiu"); return; }
    ultimoRx = agoraMonotonicoUs();
    parser.alimentar(buf.data(), static_cast<size_t>(n));
    Pacote p;
    while (parser.proximo(p)) {
      if (p.tipo == kTipoPing) {
        saida.controle(serializarPacote(criarPong(tempoIphone(agoraMonotonicoUs()), p.tempo)));
      } else if (p.tipo == kTipoComando) {
        ObjetoJson o;
        std::string txt(p.dados.begin(), p.dados.end());
        if (lerObjetoJson(txt, o)) {
          auto* c = buscarJson(o, "cmd", ValorJson::Tipo::Texto);
          if (c && c->s == "keyframe") irParaChave("pedido do cliente");
        }
      }
    }
    if (parser.erro()) { log("erro de protocolo do cliente"); return; }
  }
}

void uso() {
  std::fputs(
      "uso: iphone-falso --arquivo amostra.h264 [--porta 4848] [--fps 30] [--largura 1920] [--altura 1080]\n"
      "       [--desvio-relogio-us N] [--perder-quadro N] [--atraso-ms N] [--aparelho X] [--nome X] [--id X] [--silencioso]\n"
      "       [--preset-proprio 1080p60] [--bitrate-proprio bps]   qualidade salva no iPhone (handshake sem preset/bitrate)\n"
      "       [--trocar-preset-apos-s N --para 720p30 [--para-bitrate bps] [--arquivo-troca outra.h264]]   troca no meio da conexao\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : (uso(), std::exit(2), nullptr); };
    if (a == "--porta") g.porta = static_cast<uint16_t>(std::atoi(val()));
    else if (a == "--arquivo") g.arquivo = val();
    else if (a == "--fps") g.fps = std::atof(val());
    else if (a == "--largura") g.largura = std::atoi(val());
    else if (a == "--altura") g.altura = std::atoi(val());
    else if (a == "--desvio-relogio-us") g.desvioUs = std::atoll(val());
    else if (a == "--perder-quadro") g.perderQuadro = std::atoi(val());
    else if (a == "--atraso-ms") g.atrasoMs = std::atoi(val());
    else if (a == "--travar-apos-s") g.travarAposS = std::atof(val());
    else if (a == "--travar-por-s") g.travarPorS = std::atof(val());
    else if (a == "--preset-proprio") g.presetProprio = val();
    else if (a == "--bitrate-proprio") g.bitrateProprio = std::atoll(val());
    else if (a == "--trocar-preset-apos-s") g.trocarAposS = std::atof(val());
    else if (a == "--para") g.trocarPara = val();
    else if (a == "--para-bitrate") g.trocarBitrate = std::atoll(val());
    else if (a == "--arquivo-troca") g.arquivoTroca = val();
    else if (a == "--aparelho") g.aparelho = val();
    else if (a == "--nome") g.nome = val();
    else if (a == "--id") g.id = val();
    else if (a == "--silencioso") g.silencioso = true;
    else { uso(); return 2; }
  }
  if (g.arquivo.empty() || g.fps <= 0) { uso(); return 2; }
  std::ifstream f(g.arquivo, std::ios::binary);
  if (!f) { std::fprintf(stderr, "nao abriu %s\n", g.arquivo.c_str()); return 2; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  g_fluxo = separarAnnexB(bytes);
  if (g_fluxo.quadros.empty() || g_fluxo.config.empty() || !g_fluxo.quadros[0].chave) {
    std::fputs("arquivo sem SPS/PPS ou sem quadro-chave no inicio\n", stderr);
    return 2;
  }
  Formato teste;
  if ((!g.presetProprio.empty() && !lerPreset(g.presetProprio, teste)) ||
      (g.trocarAposS > 0 && !lerPreset(g.trocarPara, teste))) {
    std::fputs("--preset-proprio ou --para invalido (ex.: 1080p60)\n", stderr);
    return 2;
  }
  if (!g.arquivoTroca.empty()) {
    std::ifstream ft(g.arquivoTroca, std::ios::binary);
    if (!ft) { std::fprintf(stderr, "nao abriu %s\n", g.arquivoTroca.c_str()); return 2; }
    std::vector<uint8_t> bt((std::istreambuf_iterator<char>(ft)), std::istreambuf_iterator<char>());
    g_fluxoTroca = separarAnnexB(bt);
    if (g_fluxoTroca.quadros.empty() || g_fluxoTroca.config.empty() || !g_fluxoTroca.quadros[0].chave) {
      std::fputs("arquivo da troca sem SPS/PPS ou sem quadro-chave no inicio\n", stderr);
      return 2;
    }
  }
  iniciarRede();
  std::string erro;
  Soquete escuta = escutarTcp(g.porta, erro);
  if (escuta == kSoqueteInvalido) { std::fprintf(stderr, "%s\n", erro.c_str()); return 1; }
  std::printf("ESCUTANDO %u (%zu quadros, config %zu bytes)\n", unsigned(g.porta), g_fluxo.quadros.size(),
              g_fluxo.config.size());
  std::fflush(stdout);
  for (;;) {
    Soquete c = aceitar(escuta, 200);
    if (c == kSoqueteInvalido) continue;
    std::thread([c] {
      sessao(c);
      fecharSoquete(c);
    }).detach();
  }
}
