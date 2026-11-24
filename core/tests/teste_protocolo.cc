#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "protocolo.h"
#include "teste.h"

using namespace fmappcam;

namespace fmappcam {
static std::ostream& operator<<(std::ostream& o, ErroHandshake e) { return o << int(e); }
}  // namespace fmappcam

namespace {
std::string hex(const std::vector<uint8_t>& v) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ' ';
    s += d[v[i] >> 4];
    s += d[v[i] & 15];
  }
  return s;
}
}  // namespace

TESTE(vetor_dourado_quadro) {
  Pacote p;
  p.tipo = 2;
  p.tempo = 0x0102030405060708ULL;
  p.dados = {0x01, 0x00, 0x00, 0x00, 0x01, 0x65};
  CHECAR_IGUAIS(hex(serializarPacote(p)), std::string("02 01 02 03 04 05 06 07 08 00 00 00 06 01 00 00 00 01 65"));
}

TESTE(vetor_dourado_pong) {
  Pacote p = criarPong(1000000, 123456);
  CHECAR_IGUAIS(hex(serializarPacote(p)), std::string("0A 00 00 00 00 00 0F 42 40 00 00 00 08 00 00 00 00 00 01 E2 40"));
}

TESTE(pacote_ida_e_volta) {
  Pacote p;
  p.tipo = kTipoStatus;
  p.tempo = 0xFFFFFFFFFFFFFFFFULL;
  p.dados = {1, 2, 3, 250};
  auto b = serializarPacote(p);
  ParserPacotes ps;
  ps.alimentar(b.data(), b.size());
  Pacote q;
  CHECAR(ps.proximo(q));
  CHECAR_IGUAIS(int(q.tipo), int(kTipoStatus));
  CHECAR(q.tempo == p.tempo);
  CHECAR(q.dados == p.dados);
  CHECAR(!ps.proximo(q));
  CHECAR(!ps.erro());
}

TESTE(pacote_vazio) {
  auto b = serializarPacote(criarPing(42));
  CHECAR_IGUAIS(b.size(), size_t(13));
  ParserPacotes ps;
  ps.alimentar(b.data(), b.size());
  Pacote q;
  CHECAR(ps.proximo(q));
  CHECAR(q.dados.empty());
  CHECAR(q.tempo == 42);
}

TESTE(pacote_cortado_byte_a_byte) {
  std::vector<uint8_t> fluxo;
  for (int i = 0; i < 5; ++i) {
    Pacote p = criarQuadro(1000 + uint64_t(i), i == 0, reinterpret_cast<const uint8_t*>("abcdefghij"), 3 + size_t(i) * 2);
    auto b = serializarPacote(p);
    fluxo.insert(fluxo.end(), b.begin(), b.end());
  }
  ParserPacotes ps;
  int achados = 0;
  for (uint8_t byte : fluxo) {
    ps.alimentar(&byte, 1);
    Pacote q;
    while (ps.proximo(q)) {
      CHECAR(q.tempo == 1000 + uint64_t(achados));
      CHECAR_IGUAIS(q.dados.size(), size_t(1 + 3 + achados * 2));
      ++achados;
    }
  }
  CHECAR_IGUAIS(achados, 5);
  CHECAR(!ps.erro());
}

TESTE(pacote_cortado_em_todo_ponto) {
  auto a = serializarPacote(criarQuadro(7, true, reinterpret_cast<const uint8_t*>("xyz"), 3));
  auto b = serializarPacote(criarPing(9));
  std::vector<uint8_t> f(a);
  f.insert(f.end(), b.begin(), b.end());
  for (size_t corte = 0; corte <= f.size(); ++corte) {
    ParserPacotes ps;
    ps.alimentar(f.data(), corte);
    ps.alimentar(f.data() + corte, f.size() - corte);
    Pacote q;
    int n = 0;
    while (ps.proximo(q)) ++n;
    CHECAR_IGUAIS(n, 2);
  }
}

TESTE(tamanho_acima_do_maximo_e_erro) {
  std::vector<uint8_t> b = {kTipoQuadro, 0, 0, 0, 0, 0, 0, 0, 1};
  escreverU32BE(b, kTamanhoMaximoPacote + 1);
  ParserPacotes ps;
  ps.alimentar(b.data(), b.size());
  Pacote q;
  CHECAR(!ps.proximo(q));
  CHECAR(ps.erro());
}

TESTE(tamanho_igual_ao_maximo_e_aceito) {
  std::vector<uint8_t> b = {kTipoStatus, 0, 0, 0, 0, 0, 0, 0, 1};
  escreverU32BE(b, kTamanhoMaximoPacote);
  ParserPacotes ps;
  ps.alimentar(b.data(), b.size());
  Pacote q;
  CHECAR(!ps.proximo(q));
  CHECAR(!ps.erro());  // so esta esperando os dados
}

TESTE(tipo_desconhecido_e_pulado) {
  std::vector<uint8_t> f;
  Pacote desc{77, 5, std::vector<uint8_t>(1000, 0xAB)};
  auto d = serializarPacote(desc);
  f.insert(f.end(), d.begin(), d.end());
  auto ok = serializarPacote(criarPing(11));
  f.insert(f.end(), ok.begin(), ok.end());
  // em pedacinhos de 7 bytes, o desconhecido atravessa varios
  ParserPacotes ps;
  int n = 0;
  for (size_t i = 0; i < f.size(); i += 7) {
    ps.alimentar(f.data() + i, std::min<size_t>(7, f.size() - i));
    Pacote q;
    while (ps.proximo(q)) {
      CHECAR_IGUAIS(int(q.tipo), int(kTipoPing));
      CHECAR(q.tempo == 11);
      ++n;
    }
  }
  CHECAR_IGUAIS(n, 1);
  CHECAR(!ps.erro());
}

TESTE(handshake_pedido_valido) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"cliente\":\"obs 0.1\",\"preset\":\"720p60\",\"lente\":\"tele\",\"bitrate\":5000000}", p),
                ErroHandshake::Nenhum);
  CHECAR_IGUAIS(p.preset, std::string("720p60"));
  CHECAR_IGUAIS(p.lente, std::string("tele"));
  CHECAR(p.bitrate == 5000000);
  CHECAR_IGUAIS(p.cliente, std::string("obs 0.1"));
}

TESTE(handshake_pedido_so_versao_usa_padroes) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1}", p), ErroHandshake::Nenhum);
  // preset e bitrate ausentes = o iPhone usa a qualidade salva nele (vazio e 0 aqui)
  CHECAR(p.preset.empty());
  CHECAR_IGUAIS(p.lente, std::string("wide"));
  CHECAR(p.bitrate == 0);
}

TESTE(handshake_sem_preset_nem_bitrate_quando_pedido_do_iphone) {
  PedidoHandshake p;
  p.cliente = "x";
  p.preset.clear();
  p.bitrate = 0;
  p.lente.clear();
  std::string l = montarPedidoHandshake(p);
  CHECAR(l.find("preset") == std::string::npos);
  CHECAR(l.find("bitrate") == std::string::npos);
  CHECAR(l.find("lente") == std::string::npos);
  CHECAR_IGUAIS(l, std::string("{\"v\":1,\"cliente\":\"x\"}\n"));
  // so o preset fixado: bitrate continua ausente, e o contrario tambem
  p.preset = "720p60";
  l = montarPedidoHandshake(p);
  CHECAR(l.find("\"preset\":\"720p60\"") != std::string::npos && l.find("bitrate") == std::string::npos);
  p.preset.clear();
  p.bitrate = 6000000;
  l = montarPedidoHandshake(p);
  CHECAR(l.find("preset") == std::string::npos && l.find("\"bitrate\":6000000") != std::string::npos);
  // ida e volta: o que foi omitido volta como ausente
  l.pop_back();
  PedidoHandshake q;
  CHECAR_IGUAIS(lerPedidoHandshake(l, q), ErroHandshake::Nenhum);
  CHECAR(q.preset.empty() && q.bitrate == 6000000);
}

TESTE(handshake_campo_invalido_opcional_cai_no_padrao) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"preset\":\"4k\",\"lente\":5,\"bitrate\":99,\"extra\":[1,2]}", p),
                ErroHandshake::Nenhum);
  CHECAR_IGUAIS(p.preset, std::string("1080p30"));
  CHECAR_IGUAIS(p.lente, std::string("wide"));
  CHECAR(p.bitrate == 10000000);
}

TESTE(handshake_invalido) {
  PedidoHandshake p;
  const char* ruins[] = {"", "lixo", "{", "{\"v\":1", "{\"v\":}", "{\"preset\":\"1080p30\"}", "{\"v\":\"1\"}",
                         "{\"v\":1.5}", "{\"v\":1} x", "[1]", "{\"v\":{\"a\":1}}", "{\"v\":1,}",
                         "{\"v\":1,\"a\":\"\\q\"}", "{\"v\":NaN}", "{\"v\":1,\"a\":\"sem fim}"};
  for (const char* r : ruins) {
    CHECAR_IGUAIS(lerPedidoHandshake(r, p), ErroHandshake::Handshake);
  }
}

TESTE(handshake_versao_desconhecida) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":2}", p), ErroHandshake::Versao);
  CHECAR_IGUAIS(p.v, 2);
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":0}", p), ErroHandshake::Versao);
}

TESTE(handshake_linha_longa) {
  PedidoHandshake p;
  std::string longa = "{\"v\":1,\"cliente\":\"" + std::string(5000, 'a') + "\"}";
  CHECAR_IGUAIS(lerPedidoHandshake(longa, p), ErroHandshake::Handshake);
  std::string limite = "{\"v\":1,\"cliente\":\"" + std::string(4096 - 1 - 18 - 2, 'a') + "\"}";
  CHECAR_IGUAIS(limite.size() + 1, size_t(4096));
  CHECAR_IGUAIS(lerPedidoHandshake(limite, p), ErroHandshake::Nenhum);
}

TESTE(handshake_pedido_ida_e_volta) {
  PedidoHandshake a;
  a.cliente = "OBS Cam Mobile Extension 1.0.0 \"x\"";
  a.preset = "1080p60";
  a.bitrate = 20000000;
  std::string l = montarPedidoHandshake(a);
  CHECAR(l.back() == '\n');
  l.pop_back();
  PedidoHandshake b;
  CHECAR_IGUAIS(lerPedidoHandshake(l, b), ErroHandshake::Nenhum);
  CHECAR_IGUAIS(b.cliente, a.cliente);
  CHECAR_IGUAIS(b.preset, a.preset);
  CHECAR(b.bitrate == a.bitrate);
}

TESTE(handshake_resposta_com_nome_e_id) {
  RespostaHandshake r;
  r.ok = true;
  r.aparelho = "iPhone 13";
  r.nome = "Camera \"Altar\" \xC3\xA7";
  r.id = "6f1d2c3a-0000-4000-8000-123456789abc";
  r.w = 1920;
  r.h = 1080;
  r.fps = 30;
  std::string l = montarRespostaHandshake(r);
  l.pop_back();
  RespostaHandshake s;
  CHECAR(lerRespostaHandshake(l, s));
  CHECAR(s.ok);
  CHECAR_IGUAIS(s.nome, r.nome);
  CHECAR_IGUAIS(s.id, r.id);
  CHECAR_IGUAIS(s.w, 1920);
  CHECAR_IGUAIS(s.codec, std::string("h264"));
  CHECAR(s.fps == 30);
}

TESTE(handshake_resposta_sem_nome_id_ainda_vale) {
  RespostaHandshake s;
  CHECAR(lerRespostaHandshake("{\"ok\":true,\"v\":1,\"aparelho\":\"iPhone 13\",\"w\":1920,\"h\":1080,\"fps\":30,\"codec\":\"h264\",\"novo\":true}", s));
  CHECAR(s.nome.empty() && s.id.empty());
}

TESTE(handshake_resposta_erros) {
  RespostaHandshake e;
  e.erro = "versao";
  e.vSuportadas = {1, 2};
  std::string l = montarRespostaHandshake(e);
  CHECAR(l.find("\"v_suportadas\":[1,2]") != std::string::npos);
  l.pop_back();
  RespostaHandshake s;
  CHECAR(lerRespostaHandshake(l, s));
  CHECAR(!s.ok);
  CHECAR_IGUAIS(s.erro, std::string("versao"));
  CHECAR_IGUAIS(s.vSuportadas.size(), size_t(2));
  CHECAR(!lerRespostaHandshake("{\"ok\":false}", s));
  CHECAR(!lerRespostaHandshake("nada", s));
}

TESTE(json_unicode_e_escapes) {
  ObjetoJson o;
  CHECAR(lerObjetoJson("{\"a\":\"\\u00e7\\u4e2d\\ud83d\\ude00\\n\",\"b\":[1,2.5,-3e2],\"c\":null,\"d\":true}", o));
  CHECAR_IGUAIS(o["a"].s, std::string("\xC3\xA7\xE4\xB8\xAD\xF0\x9F\x98\x80\n"));
  CHECAR_IGUAIS(o["b"].arr.size(), size_t(3));
  CHECAR(o["b"].arr[2] == -300);
  CHECAR(o["d"].b);
  CHECAR(!lerObjetoJson("{\"a\":\"\\ud83d\"}", o));  // substituto sozinho
}

TESTE(json_lixo_nao_derruba) {
  ObjetoJson o;
  std::string base = "{\"v\":1,\"x\":[1,2],\"s\":\"ab\\u00e7\"}";
  for (size_t n = 0; n < base.size(); ++n) lerObjetoJson(base.substr(0, n), o);  // truncado, so nao pode quebrar
  for (int i = 0; i < 256; ++i) {
    std::string s = base;
    s[size_t(i) % s.size()] = char(i);
    lerObjetoJson(s, o);
  }
  CHECAR(!lerObjetoJson(std::string("{\"a\":\"\0\"}", 9), o));
  CHECAR(lerObjetoJson("  { }  ", o));
}

TESTE(sincronia_com_desvio_conhecido) {
  // iPhone 5 s a frente do cliente, ida 3 ms e volta 3 ms
  const int64_t desvio = 5000000;
  SincroniaRelogio s;
  CHECAR(!s.pronta());
  uint64_t t0 = 100000000;
  for (int i = 0; i < 10; ++i) {
    uint64_t t1 = t0 + 6000;
    uint64_t ts = uint64_t(int64_t(t0 + 3000) + desvio);
    s.registrar(t0, t1, ts);
    t0 += 1000000;
  }
  CHECAR(s.pronta());
  CHECAR_IGUAIS(s.diferenca(), desvio);
  // quadro capturado (relogio do iPhone) 10 ms antes de chegar
  uint64_t chegada = 200000000;
  uint64_t tempoQuadro = uint64_t(int64_t(chegada - 10000) + desvio);
  CHECAR_IGUAIS(s.atraso(chegada, tempoQuadro), int64_t(10000));
}

TESTE(sincronia_escolhe_menor_ida_e_volta) {
  const int64_t desvio = -777000;  // iPhone atras
  SincroniaRelogio s;
  // amostras ruins (rede lenta e assimetrica) e uma boa
  s.registrar(1000000, 1000000 + 80000, uint64_t(int64_t(1000000 + 70000) + desvio));  // ida 70, volta 10
  s.registrar(2000000, 2000000 + 2000, uint64_t(int64_t(2000000 + 1000) + desvio));    // simetrica
  s.registrar(3000000, 3000000 + 50000, uint64_t(int64_t(3000000 + 5000) + desvio));
  CHECAR_IGUAIS(s.idaEVolta(), int64_t(2000));
  CHECAR_IGUAIS(s.diferenca(), desvio);
}

TESTE(sincronia_janela_de_10) {
  SincroniaRelogio s;
  s.registrar(0, 100, 50);  // ida 100, melhor de todas
  for (int i = 1; i <= 10; ++i) s.registrar(uint64_t(i) * 1000000, uint64_t(i) * 1000000 + 5000, uint64_t(i) * 1000000 + 2500);
  CHECAR_IGUAIS(s.idaEVolta(), int64_t(5000));  // a primeira saiu da janela
}

// ---- Regras do texto (docs/PROTOCOLO.md, secao 1) e relogio hostil ----

TESTE(json_pula_valor_desconhecido_de_qualquer_tipo) {
  ObjetoJson o;
  CHECAR(lerObjetoJson("{\"a\":{\"b\":{\"c\":[1,{\"d\":null}]}},\"l\":[{\"id\":\"wide\"}],\"s\":[\"x\",\"y\"],\"m\":[1,\"x\"],\"n\":[[1],[2]],\"ok\":true}", o));
  CHECAR(buscarJson(o, "ok", ValorJson::Tipo::Bool) != nullptr);
  CHECAR(buscarJson(o, "a", ValorJson::Tipo::Numero) == nullptr);
  CHECAR(buscarJson(o, "l", ValorJson::Tipo::ArrayNumeros) == nullptr);
  CHECAR(o.count("l") == 1);  // a chave existe (conta para chave repetida), so o valor e ignorado
  // lista de numeros maior que 1024 e valor desconhecido valido, nao erro
  std::string grande = "{\"v\":1,\"x\":[";
  for (int i = 0; i < 2000; ++i) grande += "1,";
  grande += "1]}";
  CHECAR(lerObjetoJson(grande, o));
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake(grande, p), ErroHandshake::Nenhum);
  // lista de ate 1024 numeros continua lista de numeros
  std::string mil = "[";
  for (int i = 0; i < 1023; ++i) mil += "1,";
  mil += "1]";
  CHECAR(lerObjetoJson("{\"x\":" + mil + "}", o));
  CHECAR_IGUAIS(o["x"].arr.size(), size_t(1024));
}

TESTE(json_profundidade_segura_contra_pilha) {
  ObjetoJson o;
  auto aninhado = [](int d, char a, char f) { return std::string(size_t(d), a) + std::string(size_t(d), f); };
  CHECAR(lerObjetoJson("{\"x\":" + aninhado(60, '[', ']') + "}", o));
  CHECAR(!lerObjetoJson("{\"x\":" + aninhado(100000, '[', ']') + "}", o));  // nao estoura a pilha
  std::string objs = "{\"x\":";
  for (int i = 0; i < 100000; ++i) objs += "{\"a\":";
  objs += "1";
  for (int i = 0; i < 100000; ++i) objs += "}";
  objs += "}";
  CHECAR(!lerObjetoJson(objs, o));
}

TESTE(handshake_campo_futuro_aninhado_vale_nos_dois_lados) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"lentes\":[{\"id\":\"wide\"}],\"extra\":{\"a\":{\"b\":[]}},\"preset\":\"720p60\"}", p),
                ErroHandshake::Nenhum);
  CHECAR_IGUAIS(p.preset, std::string("720p60"));
  RespostaHandshake r;
  CHECAR(lerRespostaHandshake("{\"ok\":true,\"v\":1,\"aparelho\":\"iPhone\",\"w\":1920,\"h\":1080,\"fps\":30,\"codec\":\"h264\",\"lentes\":[{\"id\":\"wide\"}],\"caps\":{\"hdr\":true}}", r));
  CHECAR(r.ok);
  CHECAR_IGUAIS(r.w, 1920);
  CHECAR_IGUAIS(r.aparelho, std::string("iPhone"));
  // comando com campo desconhecido continua valendo
  ObjetoJson c;
  CHECAR(lerObjetoJson("{\"cmd\":\"keyframe\",\"opts\":{}}", c));
  CHECAR(buscarJson(c, "cmd", ValorJson::Tipo::Texto) != nullptr);
}

TESTE(handshake_chave_repetida_bom_e_utf8_invalido) {
  PedidoHandshake p;
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"v\":1}", p), ErroHandshake::Handshake);
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"preset\":\"720p30\",\"preset\":\"720p60\"}", p), ErroHandshake::Handshake);
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"\\u0076\":2}", p), ErroHandshake::Handshake);  // mesma chave escapada
  CHECAR_IGUAIS(lerPedidoHandshake("\xEF\xBB\xBF{\"v\":1}", p), ErroHandshake::Handshake);
  const char* ruins[] = {"{\"v\":1,\"cliente\":\"\x80\"}",          "{\"v\":1,\"cliente\":\"\xC0\x80\"}",
                         "{\"v\":1,\"cliente\":\"\xC1\xBF\"}",       "{\"v\":1,\"cliente\":\"\xE0\x80\x80\"}",
                         "{\"v\":1,\"cliente\":\"\xED\xA0\x80\"}",   "{\"v\":1,\"cliente\":\"\xF4\x90\x80\x80\"}",
                         "{\"v\":1,\"cliente\":\"\xF5\x80\x80\x80\"}", "{\"v\":1,\"cliente\":\"\xE4\xB8\"}",
                         "{\"v\":1,\"cliente\":\"\xFF\xFE\"}"};
  for (const char* r : ruins) CHECAR_IGUAIS(lerPedidoHandshake(r, p), ErroHandshake::Handshake);
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1,\"cliente\":\"\xC3\xA7\xE4\xB8\xAD\xF0\x9F\x98\x80\"}", p), ErroHandshake::Nenhum);
  // o mesmo vale para a resposta e para o JSON dos tipos 3 e 4
  RespostaHandshake r;
  CHECAR(!lerRespostaHandshake("{\"ok\":true,\"ok\":true}", r));
  CHECAR(!lerRespostaHandshake("\xEF\xBB\xBF{\"ok\":true,\"v\":1}", r));
  CHECAR(!lerRespostaHandshake("{\"ok\":true,\"aparelho\":\"\xFF\"}", r));
  ObjetoJson o;
  CHECAR(!lerObjetoJson("{\"cmd\":\"keyframe\",\"cmd\":\"keyframe\"}", o));
}

TESTE(handshake_limite_4096_sem_contar_o_barra_n) {
  PedidoHandshake p;
  auto linhaDe = [](size_t n) {
    std::string s = "{\"v\":1,\"cliente\":\"";
    s += std::string(n - s.size() - 2, 'a');
    s += "\"}";
    return s;
  };
  CHECAR_IGUAIS(linhaDe(4096).size(), size_t(4096));
  CHECAR_IGUAIS(lerPedidoHandshake(linhaDe(4096), p), ErroHandshake::Nenhum);  // 4096 bytes + \n: vale
  CHECAR_IGUAIS(lerPedidoHandshake(linhaDe(4097), p), ErroHandshake::Handshake);
}

TESTE(handshake_v_ausente_nao_inteiro_ou_fora_das_versoes) {
  PedidoHandshake p;
  const char* handshake[] = {"{}", "{\"cliente\":\"x\"}", "{\"v\":null}", "{\"v\":true}", "{\"v\":\"1\"}", "{\"v\":1.5}",
                             "{\"v\":[1]}", "{\"v\":{\"a\":1}}"};
  for (const char* r : handshake) CHECAR_IGUAIS(lerPedidoHandshake(r, p), ErroHandshake::Handshake);
  const char* versao[] = {"{\"v\":0}", "{\"v\":-1}", "{\"v\":2}", "{\"v\":2000000}", "{\"v\":99999999999}",
                          "{\"v\":4294967297}", "{\"v\":1e300}", "{\"v\":1e999}", "{\"v\":-99999999999999999999}",
                          "{\"v\":1.0e2}"};
  for (const char* r : versao) CHECAR_IGUAIS(lerPedidoHandshake(r, p), ErroHandshake::Versao);
  // 4294967297 (2^32 + 1) nao pode virar a versao 1 por truncamento
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":4294967297}", p), ErroHandshake::Versao);
  CHECAR(p.v != 1);
  CHECAR_IGUAIS(lerPedidoHandshake("{\"v\":1}", p), ErroHandshake::Nenhum);
}

TESTE(resposta_numero_gigante_nao_estoura_int) {
  RespostaHandshake r;
  CHECAR(lerRespostaHandshake("{\"ok\":true,\"v\":4294967297,\"w\":1e30,\"h\":-1e30,\"fps\":1e999}", r));
  CHECAR(r.v == INT_MAX && r.w == INT_MAX && r.h == INT_MIN);
  CHECAR(std::isfinite(r.fps));
  RespostaHandshake e;
  CHECAR(lerRespostaHandshake("{\"ok\":false,\"erro\":\"versao\",\"v_suportadas\":[1,4294967297]}", e));
  CHECAR_IGUAIS(e.vSuportadas.size(), size_t(2));
  CHECAR(e.vSuportadas[0] == 1 && e.vSuportadas[1] == INT_MAX);
}

TESTE(sincronia_descarta_pong_impossivel) {
  SincroniaRelogio s;
  s.registrar(9000000, 6000000, 6001000);       // eco no futuro (t0 > t1)
  CHECAR(!s.pronta());
  s.registrar(1000000, 3500000, 2000000);       // ida e volta de 2,5 s
  CHECAR(!s.pronta());
  s.registrar(1000000, 3000001, 2000000);       // 2 s + 1 us
  CHECAR(!s.pronta());
  s.registrar(1000000, 3000000, 2000000);       // exatamente 2 s: vale
  CHECAR(s.pronta());
  CHECAR_IGUAIS(s.idaEVolta(), int64_t(2000000));
  // um pong impossivel nao pode ganhar de um bom como "menor ida e volta"
  s.registrar(9000000, 6000000, 6001000);
  CHECAR_IGUAIS(s.idaEVolta(), int64_t(2000000));
}

TESTE(sincronia_sem_estouro_com_entrada_hostil) {
  const uint64_t ext[] = {0, 1, 1000000, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull};
  for (uint64_t a : ext)
    for (uint64_t b : ext)
      for (uint64_t c : ext) {
        SincroniaRelogio s;
        s.registrar(a, b, c);
        (void)s.diferenca();
        (void)s.idaEVolta();
        (void)s.atraso(a, b);
        (void)s.atraso(c, a);
      }
  // saturacao: o tempo do iPhone muito a frente nao vira numero negativo por estouro
  SincroniaRelogio s;
  s.registrar(1000000, 1002000, 0xFFFFFFFFFFFFFFFFull);
  CHECAR_IGUAIS(s.diferenca(), INT64_MAX);
  CHECAR_IGUAIS(s.atraso(0xFFFFFFFFFFFFFFFFull, 0), INT64_MAX);
  SincroniaRelogio t;
  t.registrar(0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFFFF000ull + 2000, 0);
  CHECAR_IGUAIS(t.diferenca(), INT64_MIN);
  CHECAR_IGUAIS(t.atraso(0, 0xFFFFFFFFFFFFFFFFull), INT64_MIN);
  // relogios normais seguem exatos
  SincroniaRelogio n;
  n.registrar(1000000, 1006000, 5000000 + 1003000);
  CHECAR_IGUAIS(n.diferenca(), int64_t(5000000));
}

// Varredura 1: o pedido com 2160p30 ou 1080p120 (que a extensao oferece) virava 1080p30 no lado que le o handshake
// (iphone-falso e testes), entao o caminho 4K nunca era exercitado ponta a ponta.
TESTE(handshake_aceita_todos_os_presets_da_extensao) {
  for (const char* pr : {"720p30", "720p60", "1080p30", "1080p60", "1080p120", "2160p30"}) {
    PedidoHandshake p;
    CHECAR_IGUAIS(lerPedidoHandshake(std::string("{\"v\":1,\"preset\":\"") + pr + "\"}", p), ErroHandshake::Nenhum);
    CHECAR_IGUAIS(p.preset, std::string(pr));
  }
}
