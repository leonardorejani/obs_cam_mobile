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
#include <mutex>

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

class DescobertaWin : public DescobertaWifi {
 public:
  explicit DescobertaWin(uint32_t ifIndex) : ifIndex_(ifIndex) {}
  ~DescobertaWin() override { parar(); }

  void iniciar(AoMudar cb) override {
    aoMudar_ = std::move(cb);
    DNS_SERVICE_BROWSE_REQUEST req = {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = ifIndex_;
    req.QueryName = L"_fmcam._tcp.local";
    req.pBrowseCallback = &DescobertaWin::aoBrowse;
    req.pQueryContext = this;
    if (DnsServiceBrowse(&req, &cancel_) == DNS_REQUEST_PENDING) ativo_ = true;
  }
  void parar() override {
    if (ativo_) {
      DnsServiceBrowseCancel(&cancel_);
      ativo_ = false;
    }
  }
  std::vector<ServicoWifi> servicos() const override {
    std::lock_guard<std::mutex> l(m_);
    std::vector<ServicoWifi> r;
    for (auto& p : servicos_) {
      bool dup = false;
      for (auto& x : r) dup = dup || x.id == p.second.id;
      if (!dup) r.push_back(p.second);
    }
    return r;
  }

 private:
  static VOID WINAPI aoBrowse(DWORD status, PVOID ctx, PDNS_RECORD rec) {
    auto* d = static_cast<DescobertaWin*>(ctx);
    if (status == ERROR_SUCCESS) {
      for (PDNS_RECORD r = rec; r; r = r->pNext) {
        if (r->wType != DNS_TYPE_PTR) continue;
        std::wstring instancia = r->Data.PTR.pNameHost;
        if (r->dwTtl == 0) {  // anuncio retirado
          d->remover(utf8(instancia.c_str()));
          continue;
        }
        d->resolver(instancia);
      }
    }
    if (rec) DnsRecordListFree(rec, DnsFreeRecordList);
  }

  void resolver(const std::wstring& instancia) {
    auto* alvo = new std::pair<DescobertaWin*, std::wstring>(this, instancia);
    auto* req = new DNS_SERVICE_RESOLVE_REQUEST();
    req->Version = DNS_QUERY_REQUEST_VERSION1;
    req->InterfaceIndex = ifIndex_;
    req->QueryName = const_cast<PWSTR>(alvo->second.c_str());
    req->pResolveCompletionCallback = &DescobertaWin::aoResolve;
    req->pQueryContext = alvo;
    auto* cancel = new DNS_SERVICE_CANCEL();
    if (DnsServiceResolve(req, cancel) != DNS_REQUEST_PENDING) {
      delete alvo;
      delete req;
      delete cancel;
    }  // (req e cancel vazam por servico encontrado: poucos bytes, aceitavel ate medir no Windows)
  }

  static VOID WINAPI aoResolve(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    auto* alvo = static_cast<std::pair<DescobertaWin*, std::wstring>*>(ctx);
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
        std::lock_guard<std::mutex> l(alvo->first->m_);
        alvo->first->servicos_[utf8(alvo->second.c_str())] = s;
      }
      if (alvo->first->aoMudar_) alvo->first->aoMudar_();
    }
    if (inst) DnsServiceFreeInstance(inst);
    delete alvo;
  }

  void remover(const std::string& chave) {
    bool tinha;
    {
      std::lock_guard<std::mutex> l(m_);
      tinha = servicos_.erase(chave) > 0;
    }
    if (tinha && aoMudar_) aoMudar_();
  }

  uint32_t ifIndex_;
  AoMudar aoMudar_;
  DNS_SERVICE_CANCEL cancel_ = {};
  bool ativo_ = false;
  mutable std::mutex m_;
  std::map<std::string, ServicoWifi> servicos_;
};

}  // namespace

std::unique_ptr<DescobertaWifi> criarDescobertaWifi(uint32_t ifIndex) { return std::make_unique<DescobertaWin>(ifIndex); }

}  // namespace fmcam
#endif
