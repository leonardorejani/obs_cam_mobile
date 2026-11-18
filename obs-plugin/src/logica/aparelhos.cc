#include "aparelhos.h"

#include <algorithm>
#include <set>

#include "modelo_iphone.h"

namespace fmcam {

const char* chaveEstado(EstadoAparelho e) {
  switch (e) {
    case EstadoAparelho::SemFonte: return "sem_fonte";
    case EstadoAparelho::Procurando: return "procurando";
    case EstadoAparelho::Conectado: return "conectado";
    case EstadoAparelho::SemApp: return "sem_app";
    case EstadoAparelho::Ocupado: return "ocupado";
    case EstadoAparelho::SemCabo: return "sem_cabo";
    case EstadoAparelho::Falha: return "falha";
  }
  return "?";
}

std::string udidCurto(const std::string& u) {
  if (u.size() <= 14) return u;
  return u.substr(0, 8) + ".." + u.substr(u.size() - 4);
}

std::string rotuloAparelho(const InfoAparelho& i) {
  if (!i.nome.empty()) return i.modelo.empty() || i.modelo == i.nome ? i.nome : i.nome + " (" + i.modelo + ")";
  if (!i.modelo.empty()) return i.modelo + " " + udidCurto(i.udid);
  return "iPhone " + udidCurto(i.udid);
}

RegistroAparelhos::RegistroAparelhos(std::string caminho, std::unique_ptr<DescobertaWifi> wifi)
    : caminho_(std::move(caminho)), observador_(caminho_), wifi_(std::move(wifi)) {}
RegistroAparelhos::~RegistroAparelhos() { parar(); }

void RegistroAparelhos::iniciar() {
  observador_.iniciar([this] { notificar(); });
  if (wifi_) wifi_->iniciar([this] { notificar(); });
}
void RegistroAparelhos::parar() {
  if (wifi_) wifi_->parar();
  observador_.parar();
}
bool RegistroAparelhos::ouvindo() const { return observador_.ouvindo(); }

int RegistroAparelhos::assinar(AoMudar cb) {
  std::lock_guard<std::mutex> l(m_);
  int id = proximaAssinatura_++;
  assinantes_[id] = std::move(cb);
  return id;
}
void RegistroAparelhos::desassinar(int id) {
  std::lock_guard<std::mutex> l(m_);
  assinantes_.erase(id);
}

void RegistroAparelhos::notificar() {
  std::vector<AoMudar> cbs;
  {
    std::lock_guard<std::mutex> l(m_);
    for (auto& p : assinantes_) cbs.push_back(p.second);
  }
  for (auto& cb : cbs) cb();
}

InfoAparelho RegistroAparelhos::montar(const std::string& udid, const Dados* d, bool noCabo) const {
  InfoAparelho i;
  if (d) i = d->info;
  i.udid = udid;
  i.noCabo = noCabo;
  // Sem entrada USB mas com a entrada de rede do mesmo UDID, a sessao segue conectada pelo usbmuxd: nao e "fora do cabo".
  if (!noCabo && i.fontes > 0 && udid.rfind("wifi:", 0) != 0 && !temEntradaRede(udid) && (i.estado == EstadoAparelho::Conectado || i.estado == EstadoAparelho::Procurando)) {
    // cabo saiu e a sessao ainda nao percebeu: o cabo e a verdade (principio: fonte viva)
    i.estado = EstadoAparelho::SemCabo;
  }
  if (i.fontes == 0) i.estado = EstadoAparelho::SemFonte;
  return i;
}

bool RegistroAparelhos::temEntradaRede(const std::string& udid) const {
  for (auto& d : observador_.todos())
    if (d.conexao == "Network" && d.serial == udid) return true;
  return false;
}

std::vector<ServicoWifi> RegistroAparelhos::servicosWifi() const {
  return wifi_ ? wifi_->servicos() : std::vector<ServicoWifi>{};
}

std::string RegistroAparelhos::rotulo(const std::string& valor) const {
  auto lista = listar();
  std::vector<IphoneParaRotulo> ids;
  for (auto& i : lista) ids.push_back({i.udid, nomeComercialIphone(i.modelo)});
  auto r = rotulosIphones(ids);
  auto it = r.find(valor);
  if (it != r.end()) return it->second;
  if (valor.rfind("wifi:", 0) == 0) {  // o Wi-Fi do mesmo iPhone que ja esta no cabo: o rotulo e o do cabo
    const std::string id = valor.substr(5);
    for (auto& i : lista)
      if (!i.idApp.empty() && i.idApp == id) return r[i.udid];
  }
  return "";
}

std::vector<InfoAparelho> RegistroAparelhos::listar() const {
  auto cabo = observador_.dispositivos();
  std::lock_guard<std::mutex> l(m_);
  std::vector<InfoAparelho> r;
  std::set<std::string> vistos;
  for (auto& d : cabo) {
    if (!vistos.insert(d.serial).second) continue;
    auto it = dados_.find(d.serial);
    r.push_back(montar(d.serial, it == dados_.end() ? nullptr : &it->second, true));
  }
  if (wifi_) {
    std::set<std::string> idsNoCabo;
    for (auto& i : r)
      if (!i.idApp.empty()) idsNoCabo.insert(i.idApp);
    for (auto& sv : wifi_->servicos()) {
      if (idsNoCabo.count(sv.id)) continue;  // mesmo iPhone no cabo e no Wi-Fi: so o do cabo
      std::string chave = "wifi:" + sv.id;
      auto it = dados_.find(chave);
      InfoAparelho i = montar(chave, it == dados_.end() ? nullptr : &it->second, false);
      i.wifi = true;
      i.ip = sv.ip;
      if (i.nome.empty()) i.nome = sv.nome;
      if (i.idApp.empty()) i.idApp = sv.id;
      if (i.fontes > 0 && i.estado == EstadoAparelho::SemCabo) i.estado = EstadoAparelho::Procurando;
      vistos.insert(chave);
      r.push_back(i);
    }
  }
  for (auto& p : dados_)
    if (!vistos.count(p.first) && p.second.info.fontes > 0) r.push_back(montar(p.first, &p.second, false));
  return r;
}

bool RegistroAparelhos::buscarWifi(const std::string& id, ServicoWifi& saida) const {
  if (!wifi_) return false;
  for (auto& sv : wifi_->servicos())
    if (sv.id == id) {
      saida = sv;
      return true;
    }
  return false;
}

bool RegistroAparelhos::buscar(const std::string& udid, InfoAparelho& saida) const {
  for (auto& i : listar())
    if (i.udid == udid) {
      saida = i;
      return true;
    }
  return false;
}

std::string RegistroAparelhos::nomeConhecido(const std::string& udid) const {
  std::lock_guard<std::mutex> l(m_);
  auto it = dados_.find(udid);
  if (it == dados_.end() || it->second.info.nome.empty()) return "";
  return rotuloAparelho(it->second.info);
}

bool RegistroAparelhos::reservar(const std::string& udid, const void* dona) {
  bool mudou = false;
  {
    std::lock_guard<std::mutex> l(m_);
    Dados& d = dados_[udid];
    if (d.dona && d.dona != dona) return false;
    if (!d.dona) {
      d.dona = dona;
      d.info.udid = udid;
      d.info.fontes = 1;
      d.info.estado = EstadoAparelho::Procurando;
      mudou = true;
    }
  }
  if (mudou) notificar();
  return true;
}

void RegistroAparelhos::liberar(const std::string& udid, const void* dona) {
  {
    std::lock_guard<std::mutex> l(m_);
    auto it = dados_.find(udid);
    if (it == dados_.end() || it->second.dona != dona) return;
    it->second.dona = nullptr;
    it->second.info.fontes = 0;
    it->second.info.estado = EstadoAparelho::SemFonte;
    it->second.info.mensagem.clear();
    it->second.info.status = StatusIphone{};
  }
  notificar();
}

void RegistroAparelhos::definirIdentidade(const std::string& udid, const std::string& nome, const std::string& modelo,
                                          const std::string& idApp, int w, int h) {
  bool mudou;
  {
    std::lock_guard<std::mutex> l(m_);
    auto& i = dados_[udid].info;
    i.udid = udid;
    mudou = i.nome != nome || i.modelo != modelo || i.idApp != idApp;
    i.nome = nome;
    i.modelo = modelo;
    i.idApp = idApp;
    i.largura = w;
    i.altura = h;
  }
  if (mudou) notificar();
}

void RegistroAparelhos::definirEstado(const std::string& udid, EstadoAparelho e, const std::string& msg, bool pelaRede) {
  bool mudou;
  {
    std::lock_guard<std::mutex> l(m_);
    auto& i = dados_[udid].info;
    i.udid = udid;
    mudou = i.estado != e || i.mensagem != msg;
    mudou = mudou || i.pelaRede != pelaRede;
    i.estado = e;
    i.mensagem = msg;
    i.pelaRede = e == EstadoAparelho::Conectado && pelaRede;
    if (e != EstadoAparelho::Conectado) i.status = StatusIphone{};
  }
  if (mudou) notificar();
}

void RegistroAparelhos::definirStatus(const std::string& udid, const StatusIphone& s) {
  std::lock_guard<std::mutex> l(m_);
  dados_[udid].info.status = s;
}

}  // namespace fmcam
