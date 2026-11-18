// usbmuxd FALSO para testes: varios aparelhos, cada DeviceID tunela para um destino TCP proprio
// (um iphone-falso diferente). Fala ListDevices, Listen (Attached/Detached) e Connect.
// So POSIX (soquete Unix). Usado pelos testes e por tools/usbmuxd-falso.
#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rede.h"
#include "usbmux.h"

namespace fmappcam {

class UsbmuxdFalso {
 public:
  struct Entrada {
    DispositivoUsb dev;
    std::string destinoHost;
    uint16_t destinoPorta;
  };

  UsbmuxdFalso(std::string caminho, uint16_t portaEsperada = 4848)
      : caminho_(std::move(caminho)), portaEsperada_(portaEsperada) {}
  ~UsbmuxdFalso() { parar(); }

  bool iniciar(std::string& erro) {
    escuta_ = escutarUnix(caminho_, erro);
    if (escuta_ == kSoqueteInvalido) return false;
    th_ = std::thread([this] { aceitarLaco(); });
    return true;
  }

  void parar() {
    if (parar_.exchange(true)) return;
    if (th_.joinable()) th_.join();
    {
      std::lock_guard<std::mutex> l(m_);
      for (Soquete s : abertos_) desligarSoquete(s);
    }
    for (;;) {
      std::thread t;
      {
        std::lock_guard<std::mutex> l(m_);
        if (clientes_.empty()) break;
        t = std::move(clientes_.back());
        clientes_.pop_back();
      }
      if (t.joinable()) t.join();
    }
    fecharSoquete(escuta_);
    escuta_ = kSoqueteInvalido;
    if (!caminho_.empty()) std::remove(caminho_.c_str());
  }

  // Liga um aparelho: avisa quem esta em Listen.
  void anexar(const DispositivoUsb& d, const std::string& host, uint16_t porta) {
    std::lock_guard<std::mutex> l(m_);
    entradas_.push_back({d, host, porta});
    avisarTodos(montarPlistAttached(d));
  }
  void desanexar(int id) {
    std::lock_guard<std::mutex> l(m_);
    auto it = std::remove_if(entradas_.begin(), entradas_.end(), [&](auto& e) { return e.dev.id == id; });
    if (it == entradas_.end()) return;
    entradas_.erase(it, entradas_.end());
    for (auto& t : tuneis_)  // o usbmuxd de verdade derruba o tunel junto com a entrada
      if (t.first == id) desligarSoquete(t.second);
    avisarTodos(montarPlistDetached(id));
  }
  int conexoesConnect() const { return connects_; }
  int portaRecebida() const { return ultimaPorta_; }  // PortNumber cru do ultimo Connect

 private:
  void avisarTodos(const std::string& plist) {  // com m_ travado
    auto msg = montarMensagemUsbmux(0, plist);
    for (Soquete s : ouvintes_) enviarTudo(s, msg.data(), msg.size());
  }
  void registrar(Soquete s) {
    std::lock_guard<std::mutex> l(m_);
    abertos_.push_back(s);
  }
  void esquecer(Soquete s) {
    std::lock_guard<std::mutex> l(m_);
    abertos_.erase(std::remove(abertos_.begin(), abertos_.end(), s), abertos_.end());
    ouvintes_.erase(std::remove(ouvintes_.begin(), ouvintes_.end(), s), ouvintes_.end());
  }

  void aceitarLaco() {
    while (!parar_) {
      Soquete c = aceitar(escuta_, 100);
      if (c == kSoqueteInvalido) continue;
      registrar(c);
      std::lock_guard<std::mutex> l(m_);
      clientes_.emplace_back([this, c] { atender(c); });
    }
  }

  void responder(Soquete c, const std::string& plist) {
    auto m = montarMensagemUsbmux(1, plist);
    enviarTudo(c, m.data(), m.size());
  }

  void atender(Soquete c) {
    std::string xml, erro;
    if (lerMensagemUsbmux(c, xml, nullptr, 3000, erro)) {
      NoPlist p;
      std::string tipo;
      if (lerPlist(xml, p) && p.buscarTexto("MessageType", tipo)) {
        if (tipo == "ListDevices") {
          std::vector<DispositivoUsb> lista;
          {
            std::lock_guard<std::mutex> l(m_);
            for (auto& e : entradas_) lista.push_back(e.dev);
          }
          responder(c, montarPlistDeviceList(lista));
        } else if (tipo == "Listen") {
          responder(c, montarPlistResult(0));
          {
            std::lock_guard<std::mutex> l(m_);
            for (auto& e : entradas_) {
              auto m = montarMensagemUsbmux(0, montarPlistAttached(e.dev));
              enviarTudo(c, m.data(), m.size());
            }
            ouvintes_.push_back(c);
          }
          uint8_t b[64];
          while (!parar_) {  // fica vivo ate o cliente sair
            int r = esperarLeitura(c, 100);
            if (r < 0) break;
            if (r == 1 && lerAlgum(c, b, sizeof b) <= 0) break;
          }
        } else if (tipo == "Connect") {
          tunelar(c, p);
        }
      }
    }
    esquecer(c);
    fecharSoquete(c);
  }

  void tunelar(Soquete c, const NoPlist& p) {
    int64_t id = -1, porta = -1;
    p.buscarInteiro("DeviceID", id);
    p.buscarInteiro("PortNumber", porta);
    ++connects_;
    ultimaPorta_ = static_cast<int>(porta);
    Entrada alvo;
    bool achou = false;
    {
      std::lock_guard<std::mutex> l(m_);
      for (auto& e : entradas_)
        if (e.dev.id == id) { alvo = e; achou = true; }
    }
    if (!achou) { responder(c, montarPlistResult(2)); return; }
    if (porta != portaEmOrdemDeRede(portaEsperada_)) { responder(c, montarPlistResult(3)); return; }
    std::string erro;
    Soquete d = conectarTcp(alvo.destinoHost, alvo.destinoPorta, 2000, erro);
    if (d == kSoqueteInvalido) { responder(c, montarPlistResult(3)); return; }
    registrar(d);
    {
      std::lock_guard<std::mutex> l(m_);
      tuneis_.push_back({static_cast<int>(id), c});
    }
    responder(c, montarPlistResult(0));
    // c vira o tunel: c -> d numa thread, d -> c nesta.
    std::thread ida([c, d] {
      uint8_t b[65536];
      for (;;) {
        long n = lerAlgum(c, b, sizeof b);
        if (n <= 0 || !enviarTudo(d, b, static_cast<size_t>(n))) break;
      }
      desligarSoquete(d);
    });
    uint8_t b[65536];
    for (;;) {
      long n = lerAlgum(d, b, sizeof b);
      if (n <= 0 || !enviarTudo(c, b, static_cast<size_t>(n))) break;
    }
    desligarSoquete(c);
    ida.join();
    {
      std::lock_guard<std::mutex> l(m_);
      tuneis_.erase(std::remove_if(tuneis_.begin(), tuneis_.end(), [&](auto& t) { return t.second == c; }), tuneis_.end());
    }
    esquecer(d);
    fecharSoquete(d);
  }

  std::string caminho_;
  uint16_t portaEsperada_;
  Soquete escuta_ = kSoqueteInvalido;
  std::thread th_;
  std::atomic<bool> parar_{false};
  std::mutex m_;
  std::vector<std::thread> clientes_;
  std::vector<Soquete> abertos_;
  std::vector<Soquete> ouvintes_;
  std::vector<Entrada> entradas_;
  std::vector<std::pair<int, Soquete>> tuneis_;  // (DeviceID, soquete do cliente) dos Connect abertos
  std::atomic<int> connects_{0};
  std::atomic<int> ultimaPorta_{0};
};

}  // namespace fmappcam
