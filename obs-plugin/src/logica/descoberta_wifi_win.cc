// Bonjour no Windows 10+ com windns.h (DnsServiceBrowse/DnsServiceResolve), sem o Bonjour da Apple.
// ATENCAO: escrito sem Windows a mao. Nao foi compilado nem testado: conferir no PC da transmissao (M1).
#ifdef _WIN32
// windns.h so entrega DNS_RECORD com strings largas (PWSTR) com UNICODE definido. Vale so para este arquivo.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#include <ws2tcpip.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "descoberta_wifi.h"

namespace fmcam {
namespace {

std::string utf8(const wchar_t* w) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// Estado que os callbacks do windns tocam. Vive num shared_ptr: os callbacks rodam em threads do pool do Windows e
// podem chegar depois do ~DescobertaWin (resolucao em andamento, ultimo callback do cancel), entao nunca podem tocar
// em `this`. Depois de parar(), `vivo` fica false e nenhum callback mais avisa o dono.
struct Estado {
  std::mutex m;
  std::map<std::string, ServicoWifi> servicos;
  DescobertaWifi::AoMudar aoMudar;
  bool vivo = true;

  void notificar() {
    DescobertaWifi::AoMudar cb;
    {
      std::lock_guard<std::mutex> l(m);
      if (!vivo) return;
      cb = aoMudar;
    }
    if (cb) cb();  // fora da trava: o dono chama servicos() de dentro do aviso
  }
  void remover(const std::string& chave) {
    bool tinha;
    {
      std::lock_guard<std::mutex> l(m);
      tinha = servicos.erase(chave) > 0;
    }
    if (tinha) notificar();
  }
};
using EstadoPtr = std::shared_ptr<Estado>;

struct AlvoResolve {
  EstadoPtr estado;
  uint32_t ifIndex;
  std::wstring instancia;
};

VOID WINAPI aoResolve(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
  std::unique_ptr<AlvoResolve> alvo(static_cast<AlvoResolve*>(ctx));
  if (status == ERROR_SUCCESS && inst && inst->ip4Address) {
    ServicoWifi s;
    std::string completo = utf8(inst->pszInstanceName);
    size_t ponto = completo.find("._fmcam");
    s.nome = completo.substr(0, ponto);
    s.porta = inst->wPort;
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, inst->ip4Address, b, sizeof b);
    s.ip = b;
    for (DWORD i = 0; i < inst->dwPropertyCount; ++i) {
      std::string k = utf8(inst->keys[i]), v = utf8(inst->values[i]);
      if (k == "id") s.id = v;
      if (k == "v") s.versao = std::atoi(v.c_str());
    }
    if (s.id.empty()) s.id = s.nome;
    {
      std::lock_guard<std::mutex> l(alvo->estado->m);
      alvo->estado->servicos[utf8(alvo->instancia.c_str())] = s;
    }
    alvo->estado->notificar();
  }
  if (inst) DnsServiceFreeInstance(inst);
}

void resolver(const EstadoPtr& estado, uint32_t ifIndex, const std::wstring& instancia) {
  auto* alvo = new AlvoResolve{estado, ifIndex, instancia};
  auto* req = new DNS_SERVICE_RESOLVE_REQUEST();
  req->Version = DNS_QUERY_REQUEST_VERSION1;
  req->InterfaceIndex = ifIndex;
  req->QueryName = const_cast<PWSTR>(alvo->instancia.c_str());
  req->pResolveCompletionCallback = &aoResolve;
  req->pQueryContext = alvo;
  auto* cancel = new DNS_SERVICE_CANCEL();
  if (DnsServiceResolve(req, cancel) != DNS_REQUEST_PENDING) {
    delete alvo;
    delete req;
    delete cancel;
  }  // (req e cancel vazam por servico encontrado: poucos bytes, aceitavel ate medir no Windows)
}

// Contexto do Browse: um por iniciar(), e de proposito nunca liberado. O ultimo callback do cancel pode chegar depois
// do destrutor, e o Windows nao garante quando. Sao poucos bytes uma unica vez.
struct CtxBrowse {
  EstadoPtr estado;
  uint32_t ifIndex;
};

VOID WINAPI aoBrowse(DWORD status, PVOID ctx, PDNS_RECORD rec) {
  auto* c = static_cast<CtxBrowse*>(ctx);
  if (status == ERROR_SUCCESS) {
    for (PDNS_RECORD r = rec; r; r = r->pNext) {
      if (r->wType != DNS_TYPE_PTR) continue;
      std::wstring instancia = r->Data.PTR.pNameHost;
      if (r->dwTtl == 0) {  // anuncio retirado
        c->estado->remover(utf8(instancia.c_str()));
        continue;
      }
      resolver(c->estado, c->ifIndex, instancia);
    }
  }
  if (rec) DnsRecordListFree(rec, DnsFreeRecordList);
}

class DescobertaWin : public DescobertaWifi {
 public:
  explicit DescobertaWin(uint32_t ifIndex) : ifIndex_(ifIndex), estado_(std::make_shared<Estado>()) {}
  ~DescobertaWin() override { parar(); }

  void iniciar(AoMudar cb) override {
    if (ativo_) return;
    {
      std::lock_guard<std::mutex> l(estado_->m);
      estado_->aoMudar = std::move(cb);
      estado_->vivo = true;
    }
    DNS_SERVICE_BROWSE_REQUEST req = {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = ifIndex_;
    req.QueryName = L"_fmcam._tcp.local";
    req.pBrowseCallback = &aoBrowse;
    req.pQueryContext = new CtxBrowse{estado_, ifIndex_};
    if (DnsServiceBrowse(&req, &cancel_) == DNS_REQUEST_PENDING) ativo_ = true;
  }
  void parar() override {
    {
      std::lock_guard<std::mutex> l(estado_->m);
      estado_->vivo = false;  // callbacks que ainda chegarem nao avisam mais ninguem
      estado_->aoMudar = nullptr;
    }
    if (ativo_) {
      DnsServiceBrowseCancel(&cancel_);
      ativo_ = false;
    }
  }
  std::vector<ServicoWifi> servicos() const override {
    std::lock_guard<std::mutex> l(estado_->m);
    std::vector<ServicoWifi> r;
    for (auto& p : estado_->servicos) {
      bool dup = false;
      for (auto& x : r) dup = dup || x.id == p.second.id;
      if (!dup) r.push_back(p.second);
    }
    return r;
  }

 private:
  uint32_t ifIndex_;
  EstadoPtr estado_;
  DNS_SERVICE_CANCEL cancel_ = {};
  bool ativo_ = false;
};

}  // namespace

std::unique_ptr<DescobertaWifi> criarDescobertaWifi(uint32_t ifIndex) { return std::make_unique<DescobertaWin>(ifIndex); }

}  // namespace fmcam
#endif
