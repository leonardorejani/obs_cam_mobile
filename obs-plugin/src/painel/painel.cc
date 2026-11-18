#include "painel.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/base.h>

#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <cstdio>
#include <string>

#include "aparelhos.h"
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

 private:
  void atualizar() {
    auto* reg = registroGlobal();
    if (!reg) return;
    auto lista = reg->listar();
    // so reconstroi quando algo visivel mudou
    QString assinatura;
    for (auto& i : lista) assinatura += QString::fromStdString(linhaTexto(i)) + "|" + (i.fontes > 0 ? "1" : "0") + "\n";
    if (assinatura == ultima_) return;
    ultima_ = assinatura;
    while (QLayoutItem* it = linhas_->takeAt(0)) {
      if (QWidget* w = it->widget()) w->deleteLater();
      delete it;
    }
    vazio_->setVisible(lista.empty());
    for (auto& i : lista) linhas_->addWidget(criarLinha(i));
  }

  static std::string linhaTexto(const InfoAparelho& i) {
    return rotuloPainel(i) + "|" + (i.pelaRede ? "R" : "") + chaveEstado(i.estado) + "|" + std::to_string(int(i.status.fps + 0.5)) + "|" +
           std::to_string(int(i.status.bateria * 100)) + (i.status.carregando ? "c" : "") + "|" + i.status.temp + "|" +
           i.status.lente + "|" + std::to_string(i.status.iso) + "|" + i.status.obturador + "|" + i.status.foco + "|" +
           i.status.exposicao + "|" + i.status.branco + "|" + std::to_string(i.status.zoom) + "|" + std::to_string(i.status.bias) + "|" + std::to_string(i.status.w) + "x" +
           std::to_string(i.status.h) + "@" + std::to_string(i.status.fpsAlvo) + "|" + std::to_string(i.status.bitrate) +
           (i.status.presetObs ? "p" : "") + (i.status.bitrateObs ? "b" : "") + (i.status.qualidadeObs ? "q" : "") +
           (i.status.temBias ? "b" : "") + ((i.status.noAr || i.status.noArIphone) ? "|NOAR" : "");
  }

  QWidget* criarLinha(const InfoAparelho& i) {
    auto* quadro = new QFrame();
    quadro->setFrameShape(QFrame::StyledPanel);
    auto* h = new QHBoxLayout(quadro);
    h->setContentsMargins(8, 8, 8, 8);
    h->setSpacing(8);
    auto* ic = new QLabel();
    ic->setPixmap(icone("icone-pb-32.png", 32));
    ic->setFixedSize(32, 32);
    h->addWidget(ic);
    auto* col = new QVBoxLayout();
    col->setSpacing(2);
    auto* nome = new QLabel(QString::fromUtf8(rotuloPainel(i).c_str()));
    QFont f = nome->font();
    f.setBold(true);
    nome->setFont(f);
    col->addWidget(nome);
    QString det = i.estado == EstadoAparelho::Conectado && i.pelaRede ? QString::fromUtf8(T("Estado.ConectadoPelaRede"))
                                                                         : textoEstado(i.estado);
    if (i.estado == EstadoAparelho::Conectado) {
      det += QString(" - %1 %2").arg(int(i.status.fps + 0.5)).arg(QString::fromUtf8(T("Painel.Fps")));
      if (i.status.bateria >= 0)
        det += QString(" - %1 %2%3").arg(QString::fromUtf8(T("Painel.Bateria"))).arg(int(i.status.bateria * 100 + 0.5))
                   .arg(i.status.carregando ? " (" + QString::fromUtf8(T("Painel.Carregando")) + ")" : "");
      if (!i.status.temp.empty()) det += " - " + QString::fromUtf8(T("Painel.Temperatura")) + " " + textoTemp(i.status.temp);
    }
    auto* l2 = new QLabel(det);
    l2->setWordWrap(true);
    col->addWidget(l2);
    // leitura da camera (so o que o app mandou no status) e indicador NO AR
    QString cam;
    auto junta = [&](const char* chave, const std::string& v) {
      if (!v.empty()) cam += (cam.isEmpty() ? "" : " - ") + QString::fromUtf8(T(chave)) + " " + QString::fromStdString(v);
    };
    if (i.estado == EstadoAparelho::Conectado) {
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
        std::snprintf(b, sizeof b, mbps >= 10 ? "%.0f Mbps" : "%.1f Mbps", mbps);
        std::string t = b;
        if (i.status.bitrateObs) t += std::string(" ") + T("Painel.QualidadeObs");
        junta("Painel.Bitrate", t);
      }
      junta("Painel.Lente", i.status.lente);
      if (i.status.zoom > 0) {
        std::snprintf(b, sizeof b, "%gx", i.status.zoom);
        junta("Painel.Zoom", b);
      }
      if (i.status.iso > 0) junta("Painel.Iso", std::to_string(i.status.iso));
      junta("Painel.Obturador", i.status.obturador);
      junta("Painel.Foco", i.status.foco);
      junta("Painel.Exposicao", i.status.exposicao);
      junta("Painel.Branco", i.status.branco);
      if (i.status.temBias) {
        std::snprintf(b, sizeof b, "%+.1f EV", i.status.bias);
        junta("Painel.Bias", b);
      }
    }
    if (!cam.isEmpty()) {
      auto* l3 = new QLabel(cam);
      l3->setWordWrap(true);
      col->addWidget(l3);
    }
    if ((i.status.noAr || i.status.noArIphone) && i.estado == EstadoAparelho::Conectado) {
      auto* na = new QLabel(QString::fromUtf8(T("Painel.NoAr")));
      na->setStyleSheet("background:#d32f2f;color:white;font-weight:bold;padding:1px 6px;border-radius:3px;");
      na->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
      col->addWidget(na);
    }
    h->addLayout(col, 1);
    auto* b = new QPushButton(QString::fromUtf8(T("Painel.Adicionar")));
    b->setEnabled(i.noCabo && i.fontes == 0);
    if (i.fontes > 0) b->setToolTip(QString::fromUtf8(T("Aparelho.EmUso")));
    InfoAparelho copia = i;
    QObject::connect(b, &QPushButton::clicked, quadro, [copia] { adicionarACena(copia); });
    h->addWidget(b);
    return quadro;
  }

  QLabel* vazio_ = nullptr;
  QVBoxLayout* linhas_ = nullptr;
  QString ultima_;
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
