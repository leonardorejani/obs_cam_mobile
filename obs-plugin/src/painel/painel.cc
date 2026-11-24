#include "painel.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/base.h>

#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "aparelhos.h"
#include "config_fonte.h"
#include "modulo.h"

namespace fmcam {
namespace {

constexpr const char* kIdDock = "fmcam_painel";
#define T(s) obs_module_text(s)

QPixmap icone(const char* arquivo, int lado) {
  char* c = obs_module_file(arquivo);
  QPixmap p;
  if (c) {
    p.load(QString::fromUtf8(c));
    bfree(c);
  }
  return p.isNull() ? p : p.scaled(lado, lado, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString textoEstado(EstadoAparelho e) {
  switch (e) {
    case EstadoAparelho::SemFonte: return QString::fromUtf8(T("Estado.SemFonte"));
    case EstadoAparelho::Procurando: return QString::fromUtf8(T("Estado.Procurando"));
    case EstadoAparelho::Conectado: return QString::fromUtf8(T("Estado.Conectado"));
    case EstadoAparelho::SemApp: return QString::fromUtf8(T("Estado.SemApp"));
    case EstadoAparelho::Ocupado: return QString::fromUtf8(T("Estado.Ocupado"));
    case EstadoAparelho::SemCabo: return QString::fromUtf8(T("Estado.SemCabo"));
    case EstadoAparelho::Falha: return QString::fromUtf8(T("Estado.Falha"));
  }
  return {};
}

QString textoTemp(const std::string& t) {
  if (t.empty()) return {};
  std::string chave = "Painel.Temp." + t;
  const char* s = T(chave.c_str());
  return QString::fromUtf8(s == chave ? t.c_str() : s);
}

// "iPhone 15 Pro Max \xC2\xB7 Cabo": o mesmo rotulo da lista de aparelhos da fonte.
std::string rotuloPainel(const InfoAparelho& i) {
  std::string base;
  if (auto* reg = registroGlobal()) base = reg->rotulo(i.udid);
  if (base.empty()) base = "iPhone";
  return base + " \xC2\xB7 " + T(i.wifi ? "Aparelho.Wifi" : "Aparelho.Cabo");
}

// Cria a fonte ja apontada para o iPhone e poe na cena atual.
void adicionarACena(const InfoAparelho& i) {
  obs_source_t* cena = obs_frontend_get_current_scene();
  if (!cena) return;
  obs_scene_t* sc = obs_scene_from_source(cena);
  std::string base = "OBS Cam Mobile - " + rotuloPainel(i);
  std::string nome = base;
  for (int n = 2;; ++n) {
    obs_source_t* ex = obs_get_source_by_name(nome.c_str());
    if (!ex) break;
    obs_source_release(ex);
    nome = base + " (" + std::to_string(n) + ")";
  }
  obs_data_t* d = obs_data_create();
  obs_data_set_string(d, "udid", i.udid.c_str());
  obs_data_set_int(d, "versao_cfg", kVersaoConfig);  // fonte nova: sem migracao de valores antigos
  obs_data_set_bool(d, "encaixar", true);
  {
    std::string cache;
    if (auto* reg = registroGlobal()) cache = reg->rotulo(i.udid);
    obs_data_set_string(d, "nome_cache", cache.c_str());
  }
  obs_source_t* src = obs_source_create("fmcam_fonte", nome.c_str(), d, nullptr);
  obs_data_release(d);
  if (src) {
    obs_sceneitem_t* item = obs_scene_add(sc, src);
    // Item nasce preenchendo o canvas com bounds: trocar 1080p por 720p nao muda o tamanho na tela
    obs_video_info ovi;
    if (item && obs_get_video_info(&ovi)) {
      vec2 b = {static_cast<float>(ovi.base_width), static_cast<float>(ovi.base_height)};
      vec2 pos = {b.x / 2, b.y / 2};
      obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
      obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
      obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
      obs_sceneitem_set_bounds(item, &b);
      obs_sceneitem_set_pos(item, &pos);
    }
    blog(LOG_INFO, "[fmcam] painel: fonte '%s' adicionada a cena", nome.c_str());
    obs_source_release(src);
  }
  obs_source_release(cena);
}

// Uma linha do painel por iPhone. Os widgets nascem uma vez e so os textos e os estados mudam a cada atualizacao
// (assim um menu aberto ou um botao sendo clicado nao some no meio). Reconstroi so quando a estrutura muda.
struct Linha {
  std::string udid;
  std::string estrutura;  // o que decide quais widgets existem
  QFrame* quadro = nullptr;
  QLabel* nome = nullptr;
  QLabel* det = nullptr;
  QLabel* cam = nullptr;
  QLabel* noar = nullptr;
  QPushButton* adicionar = nullptr;
  QPushButton* testar = nullptr;
  QLabel* resultado = nullptr;
  QWidget* controles = nullptr;
  std::vector<std::pair<double, QPushButton*>> lentes;
  QPushButton* evMenos = nullptr;
  QPushButton* evMais = nullptr;
  QLabel* ev = nullptr;
  QPushButton* travaAe = nullptr;
  QPushButton* travaAf = nullptr;
  QPushButton* travaBb = nullptr;
  QComboBox* preset = nullptr;
  QComboBox* bitrate = nullptr;
  QPushButton* aplicar = nullptr;
  QLabel* appAntigo = nullptr;
  std::string ultimoPreset;  // ultimo valor do iPhone posto nos combos (so sobrescreve quando o iPhone muda)
  int64_t ultimoBitrate = -1;
  StatusIphone st;  // ultimo status visto (para os cliques)
};

std::string fmt(const char* f, double v) {
  char b[32];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

QString textoDetalhe(const InfoAparelho& i) {
  QString det = i.estado == EstadoAparelho::Conectado && i.pelaRede ? QString::fromUtf8(T("Estado.ConectadoPelaRede")) : textoEstado(i.estado);
  if (i.estado == EstadoAparelho::Conectado) {
    det += QString(" - %1 %2").arg(int(i.status.fps + 0.5)).arg(QString::fromUtf8(T("Painel.Fps")));
    if (i.status.bateria >= 0)
      det += QString(" - %1 %2%3").arg(QString::fromUtf8(T("Painel.Bateria"))).arg(int(i.status.bateria * 100 + 0.5))
                 .arg(i.status.carregando ? " (" + QString::fromUtf8(T("Painel.Carregando")) + ")" : "");
    if (!i.status.temp.empty()) det += " - " + QString::fromUtf8(T("Painel.Temperatura")) + " " + textoTemp(i.status.temp);
    if (i.status.atrasoMs >= 0)
      det += " - " + QString::fromUtf8(T("Status.Atraso")) + " " + QString::number(int(i.status.atrasoMs + 0.5)) + " ms";
  }
  return det;
}

QString textoCamera(const InfoAparelho& i) {
  QString cam;
  auto junta = [&](const char* chave, const std::string& v) {
    if (!v.empty()) cam += (cam.isEmpty() ? "" : " - ") + QString::fromUtf8(T(chave)) + " " + QString::fromStdString(v);
  };
  if (i.estado != EstadoAparelho::Conectado) return cam;
  char b[32];
  if (i.status.w > 0 && i.status.h > 0) {
    std::snprintf(b, sizeof b, "%dx%d", i.status.w, i.status.h);
    std::string f = b;
    if (i.status.fpsAlvo > 0) {
      std::snprintf(b, sizeof b, " @ %g", i.status.fpsAlvo);
      f += b;
    }
    if (i.status.presetObs) f += std::string(" ") + T("Painel.QualidadeObs");  // "(OBS)" so no item que o OBS fixou
    junta("Painel.Formato", f);
  }
  if (i.status.bitrate > 0) {
    double mbps = i.status.bitrate / 1e6;
    std::string t = fmt(mbps >= 10 ? "%.0f Mbps" : "%.1f Mbps", mbps);
    if (i.status.bitrateObs) t += std::string(" ") + T("Painel.QualidadeObs");
    junta("Painel.Bitrate", t);
  }
  junta("Painel.Lente", i.status.lente);
  if (i.status.zoom > 0) junta("Painel.Zoom", fmt("%gx", i.status.zoom));
  if (i.status.iso > 0) junta("Painel.Iso", std::to_string(i.status.iso));
  junta("Painel.Obturador", i.status.obturador);
  junta("Painel.Foco", i.status.foco);
  junta("Painel.Exposicao", i.status.exposicao);
  junta("Painel.Branco", i.status.branco);
  if (i.status.temBias) junta("Painel.Bias", fmt("%+.1f EV", i.status.bias));
  return cam;
}

QString textoTeste(const InfoAparelho& i) {
  ResultadoTeste r = avaliarTeste(i.status, i.estado == EstadoAparelho::Conectado);
  const char* nivel = r.nivel == ResultadoTeste::Nivel::Ok ? "Teste.Ok"
                      : r.nivel == ResultadoTeste::Nivel::Atencao ? "Teste.Atencao"
                      : r.nivel == ResultadoTeste::Nivel::Ruim ? "Teste.Ruim" : "Teste.SemDados";
  QString t = QString::fromUtf8(T(nivel));
  if (r.nivel == ResultadoTeste::Nivel::SemDados) return t;
  t += ". " + QString::fromUtf8(T("Teste.Atraso")) + QString(" %1 ms (p95 %2 ms)").arg(int(i.status.atrasoMs + 0.5)).arg(int(i.status.atrasoP95Ms + 0.5));
  if (i.status.idaEVoltaMs >= 0)
    t += ", " + QString::fromUtf8(T("Teste.IdaEVoltaTexto")) + QString(" %1 ms").arg(i.status.idaEVoltaMs, 0, 'f', 1);
  t += QString(", %1/%2 fps").arg(int(i.status.fps + 0.5)).arg(int(i.status.fpsAlvo + 0.5));
  if (!r.avisos.empty()) {
    t += ". ";
    for (size_t k = 0; k < r.avisos.size(); ++k) t += (k ? ", " : "") + QString::fromUtf8(T(r.avisos[k].c_str()));
  }
  return t + ".";
}

bool temControles(const StatusIphone& s) { return s.temTravas || !s.lentes.empty() || s.temFaixaEv || !s.preset.empty(); }

class PainelFmCam : public QWidget {
 public:
  PainelFmCam() {
    auto* raiz = new QVBoxLayout(this);
    raiz->setContentsMargins(8, 8, 8, 8);
    raiz->setSpacing(8);
    auto* cab = new QHBoxLayout();
    auto* ic = new QLabel();
    ic->setPixmap(icone("icone-pb-64.png", 32));
    ic->setFixedSize(32, 32);
    cab->addWidget(ic);
    auto* tit = new QLabel(QString::fromUtf8(T("Painel.Titulo")));
    QFont f = tit->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 2);
    tit->setFont(f);
    cab->addWidget(tit);
    cab->addStretch(1);
    raiz->addLayout(cab);
    vazio_ = new QLabel(QString::fromUtf8(T("Painel.SemAparelhos")));
    raiz->addWidget(vazio_);
    linhas_ = new QVBoxLayout();
    linhas_->setSpacing(8);
    raiz->addLayout(linhas_);
    raiz->addStretch(1);
    auto* t = new QTimer(this);
    QObject::connect(t, &QTimer::timeout, this, [this] { atualizar(); });
    t->start(700);
    atualizar();
  }
  ~PainelFmCam() override {
    for (auto* l : linhas) delete l;
    for (auto* l : lixo_) delete l;
  }

 private:
  static void enviar(const std::string& udid, const std::string& json) {
    if (auto* reg = registroGlobal()) {
      bool ok = reg->enviarComando(udid, json);
      blog(ok ? LOG_INFO : LOG_WARNING, "[fmcam] painel: %s para %s %s", json.c_str(), udid.c_str(), ok ? "enviado" : "sem conexao");
    }
  }

  static std::string estruturaDe(const InfoAparelho& i) {
    std::string e = rotuloPainel(i) + "|" + (i.estado == EstadoAparelho::Conectado ? "c" : "-") + (temControles(i.status) ? "k" : "-");
    for (double l : i.status.lentes) e += "|" + fmt("%g", l);
    e += i.status.temFaixaEv ? "|ev" : "";
    e += i.status.temTravas ? "|tr" : "";
    e += i.status.preset.empty() ? "" : "|q";
    return e;
  }

  void atualizar() {
    auto* reg = registroGlobal();
    if (!reg) return;
    for (auto* l : lixo_) delete l;  // os widgets delas ja sairam (deleteLater) na volta anterior do laco de eventos
    lixo_.clear();
    auto lista = reg->listar();
    std::string ordem;
    for (auto& i : lista) ordem += estruturaDe(i) + "\n";
    if (ordem != ultimaOrdem_) {
      ultimaOrdem_ = ordem;
      std::map<std::string, QString> resultados;  // o resultado do Testar sobrevive a reconstrucao
      for (auto* l : linhas)
        if (l->resultado && !l->resultado->text().isEmpty()) resultados[l->udid] = l->resultado->text();
      while (QLayoutItem* it = linhas_->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
      }
      lixo_.insert(lixo_.end(), linhas.begin(), linhas.end());  // um clique ja na fila ainda pode usar a linha
      linhas.clear();
      for (auto& i : lista) {
        Linha* l = criarLinha(i);
        if (resultados.count(i.udid)) {
          l->resultado->setText(resultados[i.udid]);
          l->resultado->setVisible(true);
        }
        linhas.push_back(l);
        linhas_->addWidget(l->quadro);
      }
    }
    vazio_->setVisible(lista.empty());
    for (size_t k = 0; k < lista.size() && k < linhas.size(); ++k) preencher(linhas[k], lista[k]);
  }

  QPushButton* botao(const char* chave, QWidget* pai, bool marcavel = false) {
    auto* b = new QPushButton(QString::fromUtf8(T(chave)), pai);
    b->setCheckable(marcavel);
    return b;
  }

  Linha* criarLinha(const InfoAparelho& i) {
    auto* l = new Linha();
    l->udid = i.udid;
    l->estrutura = estruturaDe(i);
    l->quadro = new QFrame();
    l->quadro->setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(l->quadro);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    auto* h = new QHBoxLayout();
    h->setSpacing(8);
    auto* ic = new QLabel();
    ic->setPixmap(icone("icone-pb-32.png", 32));
    ic->setFixedSize(32, 32);
    h->addWidget(ic);
    auto* col = new QVBoxLayout();
    col->setSpacing(2);
    l->nome = new QLabel(QString::fromUtf8(rotuloPainel(i).c_str()));
    QFont f = l->nome->font();
    f.setBold(true);
    l->nome->setFont(f);
    col->addWidget(l->nome);
    l->det = new QLabel();
    l->det->setWordWrap(true);
    col->addWidget(l->det);
    l->cam = new QLabel();
    l->cam->setWordWrap(true);
    col->addWidget(l->cam);
    l->noar = new QLabel(QString::fromUtf8(T("Painel.NoAr")));
    l->noar->setStyleSheet("background:#d32f2f;color:white;font-weight:bold;padding:1px 6px;border-radius:3px;");
    l->noar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    col->addWidget(l->noar);
    h->addLayout(col, 1);
    auto* botoes = new QVBoxLayout();
    botoes->setSpacing(4);
    l->adicionar = botao("Painel.Adicionar", l->quadro);
    InfoAparelho copia = i;
    QObject::connect(l->adicionar, &QPushButton::clicked, l->quadro, [copia] { adicionarACena(copia); });
    botoes->addWidget(l->adicionar);
    l->testar = botao("Painel.Testar", l->quadro);
    std::string udid = i.udid;
    QObject::connect(l->testar, &QPushButton::clicked, l->quadro, [this, l, udid] { testar(l, udid); });
    botoes->addWidget(l->testar);
    botoes->addStretch(1);
    h->addLayout(botoes);
    v->addLayout(h);
    l->resultado = new QLabel();
    l->resultado->setWordWrap(true);
    l->resultado->setVisible(false);
    v->addWidget(l->resultado);

    if (i.estado == EstadoAparelho::Conectado && !temControles(i.status)) {
      l->appAntigo = new QLabel(QString::fromUtf8(T("Painel.AppAntigo")));
      l->appAntigo->setWordWrap(true);
      v->addWidget(l->appAntigo);
    }
    if (i.estado == EstadoAparelho::Conectado && temControles(i.status)) {
      l->controles = new QWidget(l->quadro);
      auto* cv = new QVBoxLayout(l->controles);
      cv->setContentsMargins(0, 0, 0, 0);
      cv->setSpacing(4);
      if (!i.status.lentes.empty()) {  // botoes de lente como na tela do iPhone
        auto* lh = new QHBoxLayout();
        lh->setSpacing(4);
        lh->addWidget(new QLabel(QString::fromUtf8(T("Painel.Lente"))));
        for (double fat : i.status.lentes) {
          std::string rot = fmt("%g", fat);
          for (auto& c : rot)
            if (c == '.') c = ',';
          auto* b = new QPushButton(QString::fromStdString(rot + "x"), l->controles);
          b->setCheckable(true);
          QObject::connect(b, &QPushButton::clicked, l->controles, [udid, fat](bool) {
            enviar(udid, "{\"cmd\":\"lente\",\"fator\":" + fmt("%g", fat) + "}");
          });
          l->lentes.emplace_back(fat, b);
          lh->addWidget(b);
        }
        lh->addStretch(1);
        cv->addLayout(lh);
      }
      auto* th = new QHBoxLayout();
      th->setSpacing(4);
      if (i.status.temFaixaEv) {
        th->addWidget(new QLabel(QString::fromUtf8(T("Painel.Ev"))));
        l->evMenos = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), l->controles);  // sinal de menos
        l->ev = new QLabel();
        l->evMais = new QPushButton("+", l->controles);
        for (auto* b : {l->evMenos, l->evMais}) b->setFixedWidth(32);
        QObject::connect(l->evMenos, &QPushButton::clicked, l->controles, [l, udid] { mudarEv(l, udid, -0.3); });
        QObject::connect(l->evMais, &QPushButton::clicked, l->controles, [l, udid] { mudarEv(l, udid, +0.3); });
        th->addWidget(l->evMenos);
        th->addWidget(l->ev);
        th->addWidget(l->evMais);
      }
      if (i.status.temTravas) {
        l->travaAe = botao("Painel.TravaAe", l->controles, true);
        l->travaAf = botao("Painel.TravaAf", l->controles, true);
        l->travaBb = botao("Painel.TravaBb", l->controles, true);
        auto liga = [udid, l](QPushButton* b, const char* cmd) {
          QObject::connect(b, &QPushButton::clicked, l->controles, [udid, b, cmd](bool marcado) {
            enviar(udid, std::string("{\"cmd\":\"") + cmd + "\",\"valor\":" + (marcado ? "true" : "false") + "}");
            (void)b;
          });
        };
        liga(l->travaAe, "trava_ae");
        liga(l->travaAf, "trava_af");
        liga(l->travaBb, "trava_bb");
        th->addWidget(l->travaAe);
        th->addWidget(l->travaAf);
        th->addWidget(l->travaBb);
      }
      th->addStretch(1);
      cv->addLayout(th);
      if (!i.status.preset.empty()) {
        auto* qh = new QHBoxLayout();
        qh->setSpacing(4);
        qh->addWidget(new QLabel(QString::fromUtf8(T("Painel.Qualidade"))));
        l->preset = new QComboBox(l->controles);
        for (const char* p : {"720p30", "720p60", "1080p30", "1080p60", "1080p120", "2160p30"}) l->preset->addItem(p, QString(p));
        l->bitrate = new QComboBox(l->controles);
        for (int64_t bps : opcoesDeBitrate(i.status.bitrate))
          l->bitrate->addItem(QString("%1 Mbps").arg(static_cast<double>(bps) / 1e6), static_cast<qlonglong>(bps));
        l->aplicar = botao("Painel.Aplicar", l->controles);
        QObject::connect(l->aplicar, &QPushButton::clicked, l->controles, [l, udid] {
          std::string j = "{\"cmd\":\"qualidade\"";
          if (l->preset->isEnabled()) j += ",\"preset\":\"" + l->preset->currentData().toString().toStdString() + "\"";
          if (l->bitrate->isEnabled()) j += ",\"bitrate\":" + std::to_string(l->bitrate->currentData().toLongLong());
          enviar(udid, j + "}");
        });
        qh->addWidget(l->preset);
        qh->addWidget(l->bitrate);
        qh->addWidget(l->aplicar);
        qh->addStretch(1);
        cv->addLayout(qh);
      }
      v->addWidget(l->controles);
    }
    return l;
  }

  static void mudarEv(Linha* l, const std::string& udid, double passo) {
    double alvo = std::round((l->st.bias + passo) * 10) / 10;
    alvo = std::max(l->st.evMin, std::min(l->st.evMax, alvo));
    enviar(udid, "{\"cmd\":\"ev\",\"valor\":" + fmt("%.1f", alvo) + "}");
  }

  void testar(Linha* l, const std::string& udid) {
    l->testar->setEnabled(false);
    l->testar->setText(QString::fromUtf8(T("Painel.Testando")));
    l->resultado->setVisible(true);
    l->resultado->setText(QString::fromUtf8(T("Painel.Testando")));
    // 3 s de video novo: as medidas da extensao cobrem as ultimas 3 janelas de 1 s
    QTimer::singleShot(3200, l->quadro, [this, udid] {
      InfoAparelho i;
      auto* reg = registroGlobal();
      Linha* atual = nullptr;
      for (auto* x : linhas)
        if (x->udid == udid) atual = x;
      if (!atual) return;
      QString t = reg && reg->buscar(udid, i) ? textoTeste(i) : QString::fromUtf8(T("Teste.SemDados"));
      atual->resultado->setText(t);
      atual->testar->setEnabled(true);
      atual->testar->setText(QString::fromUtf8(T("Painel.Testar")));
      blog(LOG_INFO, "[fmcam] painel: teste de %s: %s", udid.c_str(), t.toUtf8().constData());
    });
  }

  void preencher(Linha* l, const InfoAparelho& i) {
    l->st = i.status;
    l->det->setText(textoDetalhe(i));
    QString cam = textoCamera(i);
    l->cam->setText(cam);
    l->cam->setVisible(!cam.isEmpty());
    l->noar->setVisible((i.status.noAr || i.status.noArIphone) && i.estado == EstadoAparelho::Conectado);
    l->adicionar->setEnabled(i.noCabo && i.fontes == 0);
    l->adicionar->setToolTip(i.fontes > 0 ? QString::fromUtf8(T("Aparelho.EmUso")) : QString());
    l->testar->setVisible(i.estado == EstadoAparelho::Conectado || !l->resultado->text().isEmpty());
    if (!l->controles) return;
    for (auto& [fat, b] : l->lentes) b->setChecked(std::fabs(fat - i.status.fator) < 0.01);
    if (l->ev) l->ev->setText(QString::fromStdString(fmt("%+.1f", i.status.bias)));
    if (l->travaAe) {
      l->travaAe->setChecked(i.status.travaAe);
      l->travaAf->setChecked(i.status.travaAf);
      l->travaBb->setChecked(i.status.travaBb);
    }
    if (l->preset) {
      // So poe o valor do iPhone quando ele muda: a escolha que o operador ainda nao aplicou nao e desfeita.
      if (i.status.preset != l->ultimoPreset && !l->preset->view()->isVisible()) {
        l->ultimoPreset = i.status.preset;
        int k = l->preset->findData(QString::fromStdString(i.status.preset));
        if (k >= 0) l->preset->setCurrentIndex(k);
      }
      if (i.status.bitrate != l->ultimoBitrate && !l->bitrate->view()->isVisible()) {
        l->ultimoBitrate = i.status.bitrate;
        // bitrate do iPhone fora da lista: entra na lista (antes o combo ficava no item anterior, em geral 6 Mbps, e o
        // "Aplicar" de quem so trocou a resolucao mandava esse bitrate junto)
        const auto ops = opcoesDeBitrate(i.status.bitrate);
        if (l->bitrate->count() != static_cast<int>(ops.size())) {
          l->bitrate->clear();
          for (int64_t bps : ops) l->bitrate->addItem(QString("%1 Mbps").arg(static_cast<double>(bps) / 1e6), static_cast<qlonglong>(bps));
        }
        int k = l->bitrate->findData(static_cast<qlonglong>(i.status.bitrate));
        if (k >= 0) l->bitrate->setCurrentIndex(k);
      }
      // campo que a fonte do OBS fixou nao muda pelo iPhone nesta conexao (docs/PROTOCOLO.md)
      l->preset->setEnabled(!i.status.presetObs);
      l->bitrate->setEnabled(!i.status.bitrateObs);
      QString fixo = QString::fromUtf8(T("Painel.QualidadeFixadaNaFonte"));
      l->preset->setToolTip(i.status.presetObs ? fixo : QString());
      l->bitrate->setToolTip(i.status.bitrateObs ? fixo : QString());
      l->aplicar->setEnabled(!(i.status.presetObs && i.status.bitrateObs));
    }
  }

  QLabel* vazio_ = nullptr;
  QVBoxLayout* linhas_ = nullptr;
  std::string ultimaOrdem_;
  std::vector<Linha*> linhas;
  std::vector<Linha*> lixo_;
};

bool g_dock = false;

void aoEventoFrontend(enum obs_frontend_event e, void*) {
  if (e != OBS_FRONTEND_EVENT_FINISHED_LOADING || g_dock) return;
  g_dock = obs_frontend_add_dock_by_id(kIdDock, T("Painel.Titulo"), new PainelFmCam());
  blog(g_dock ? LOG_INFO : LOG_WARNING, "[fmcam] painel %s", g_dock ? "criado" : "nao foi criado");
}

}  // namespace

void iniciarPainel() { obs_frontend_add_event_callback(aoEventoFrontend, nullptr); }

void pararPainel() {
  obs_frontend_remove_event_callback(aoEventoFrontend, nullptr);
  if (g_dock) obs_frontend_remove_dock(kIdDock);
  g_dock = false;
}

}  // namespace fmcam
