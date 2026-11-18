// Bonjour no Mac com dns_sd.h (API publica da Apple). Uma thread: browse + resolve + endereco, tudo por select().
#include <dns_sd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "descoberta_wifi.h"

namespace fmcam {
namespace {

class DescobertaMac : public DescobertaWifi {
 public:
  explicit DescobertaMac(uint32_t ifIndex) : ifIndex_(ifIndex) {}
  ~DescobertaMac() override { parar(); }

  void iniciar(AoMudar cb) override {
    if (th_.joinable()) return;
    aoMudar_ = std::move(cb);
    parar_ = false;
    th_ = std::thread([this] { laco(); });
  }
  void parar() override {
    parar_ = true;
    if (th_.joinable()) th_.join();
  }
  std::vector<ServicoWifi> servicos() const override {
    std::lock_guard<std::mutex> l(m_);
    std::vector<ServicoWifi> r;
    for (auto& p : servicos_) {
      bool dup = std::any_of(r.begin(), r.end(), [&](const ServicoWifi& x) { return x.id == p.second.id; });
      if (!dup) r.push_back(p.second);
    }
    return r;
  }

 private:
  struct Pendente {
    DescobertaMac* dono;
    DNSServiceRef ref = nullptr;
    bool endereco = false;  // false = resolve, true = getaddrinfo
    bool pronto = false;
    std::string chave, host;
    ServicoWifi s;
    uint32_t iface = 0;
    std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
  };

  static std::string chaveDe(const char* nome, uint32_t iface) { return std::string(nome) + "|" + std::to_string(iface); }

  static void DNSSD_API aoBrowse(DNSServiceRef, DNSServiceFlags f, uint32_t iface, DNSServiceErrorType e, const char* nome,
                                 const char* tipo, const char* dominio, void* ctx) {
    auto* d = static_cast<DescobertaMac*>(ctx);
    if (e != kDNSServiceErr_NoError) return;
    std::string chave = chaveDe(nome, iface);
    if (!(f & kDNSServiceFlagsAdd)) {
      bool tinha;
      {
        std::lock_guard<std::mutex> l(d->m_);
        tinha = d->servicos_.erase(chave) > 0;
      }
      for (auto& p : d->pend_) if (p->chave == chave) p->pronto = true;
      if (tinha) d->notificar();
      return;
    }
    auto p = std::make_unique<Pendente>();
    p->dono = d;
    p->chave = chave;
    p->s.nome = nome;
    p->iface = iface;
    if (DNSServiceResolve(&p->ref, 0, iface, nome, tipo, dominio, aoResolve, p.get()) == kDNSServiceErr_NoError)
      d->pend_.push_back(std::move(p));
  }

  static void DNSSD_API aoResolve(DNSServiceRef, DNSServiceFlags, uint32_t iface, DNSServiceErrorType e, const char*,
                                  const char* host, uint16_t porta, uint16_t txtLen, const unsigned char* txt, void* ctx) {
    auto* p = static_cast<Pendente*>(ctx);
    auto* d = p->dono;
    if (e != kDNSServiceErr_NoError || p->endereco) return;
    p->s.porta = ntohs(porta);
    uint8_t n = 0;
    if (const void* v = TXTRecordGetValuePtr(txtLen, txt, "id", &n)) p->s.id.assign(static_cast<const char*>(v), n);
    if (const void* v = TXTRecordGetValuePtr(txtLen, txt, "v", &n)) p->s.versao = std::atoi(std::string(static_cast<const char*>(v), n).c_str());
    if (p->s.id.empty()) p->s.id = p->s.nome;  // app sem TXT: o nome identifica
    p->host = host;
    p->pronto = true;  // este resolve acabou: quem vai buscar o IP e um novo pendente
    auto q = std::make_unique<Pendente>();
    q->dono = d;
    q->endereco = true;
    q->chave = p->chave;
    q->s = p->s;
    q->host = p->host;
    q->iface = iface;
    if (DNSServiceGetAddrInfo(&q->ref, 0, iface, kDNSServiceProtocol_IPv4, host, aoEndereco, q.get()) == kDNSServiceErr_NoError)
      d->novos_.push_back(std::move(q));
  }

  static void DNSSD_API aoEndereco(DNSServiceRef, DNSServiceFlags f, uint32_t, DNSServiceErrorType e, const char*,
                                   const struct sockaddr* a, uint32_t, void* ctx) {
    auto* p = static_cast<Pendente*>(ctx);
    if (e != kDNSServiceErr_NoError || !(f & kDNSServiceFlagsAdd) || !a || a->sa_family != AF_INET || p->pronto) return;
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(a)->sin_addr, b, sizeof b);
    p->s.ip = b;
    p->pronto = true;
    {
      std::lock_guard<std::mutex> l(p->dono->m_);
      p->dono->servicos_[p->chave] = p->s;
    }
    p->dono->notificar();
  }

  void notificar() {
    if (aoMudar_) aoMudar_();
  }

  void laco() {
    DNSServiceRef browse = nullptr;
    if (DNSServiceBrowse(&browse, 0, ifIndex_, kTipoServicoBonjour, nullptr, aoBrowse, this) != kDNSServiceErr_NoError) return;
    int fdB = DNSServiceRefSockFD(browse);
    while (!parar_) {
      fd_set rs;
      FD_ZERO(&rs);
      FD_SET(fdB, &rs);
      int maxfd = fdB;
      for (auto& p : pend_) {
        int fd = DNSServiceRefSockFD(p->ref);
        FD_SET(fd, &rs);
        maxfd = std::max(maxfd, fd);
      }
      timeval tv{0, 200000};
      int r = select(maxfd + 1, &rs, nullptr, nullptr, &tv);
      if (r > 0) {
        if (FD_ISSET(fdB, &rs)) DNSServiceProcessResult(browse);
        for (size_t i = 0; i < pend_.size(); ++i)  // callbacks podem empurrar para novos_, nunca para pend_
          if (FD_ISSET(DNSServiceRefSockFD(pend_[i]->ref), &rs)) DNSServiceProcessResult(pend_[i]->ref);
      }
      for (auto& q : novos_) pend_.push_back(std::move(q));
      novos_.clear();
      auto agora = std::chrono::steady_clock::now();
      for (auto it = pend_.begin(); it != pend_.end();) {
        if ((*it)->pronto || agora - (*it)->inicio > std::chrono::seconds(10)) {
          DNSServiceRefDeallocate((*it)->ref);
          it = pend_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& p : pend_) DNSServiceRefDeallocate(p->ref);
    pend_.clear();
    novos_.clear();
    DNSServiceRefDeallocate(browse);
  }

  uint32_t ifIndex_;
  AoMudar aoMudar_;
  std::thread th_;
  std::atomic<bool> parar_{false};
  mutable std::mutex m_;
  std::map<std::string, ServicoWifi> servicos_;
  std::vector<std::unique_ptr<Pendente>> pend_, novos_;  // so a thread do laco
};

}  // namespace

std::unique_ptr<DescobertaWifi> criarDescobertaWifi(uint32_t ifIndex) { return std::make_unique<DescobertaMac>(ifIndex); }

}  // namespace fmcam
