#include <obs-module.h>
#include <util/base.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include "config_fonte.h"
#include "modulo.h"
#include "sessao.h"

#ifdef FMCAM_COM_PAINEL
#include "painel.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fmcam-extension", "en-US")

namespace fmcam {
namespace {
std::unique_ptr<RegistroAparelhos> g_registro;
int g_assinatura = 0;
std::mutex g_mFontes;
std::vector<obs_source_t*> g_fontes;
}  // namespace

RegistroAparelhos* registroGlobal() { return g_registro.get(); }

void fonteViva(obs_source_t* s, bool viva) {
  std::lock_guard<std::mutex> l(g_mFontes);
  auto it = std::find(g_fontes.begin(), g_fontes.end(), s);
  if (viva && it == g_fontes.end()) g_fontes.push_back(s);
  if (!viva && it != g_fontes.end()) g_fontes.erase(it);
}

void recarregarPropriedades() {
  std::lock_guard<std::mutex> l(g_mFontes);
  for (auto* s : g_fontes) obs_source_update_properties(s);
}

}  // namespace fmcam

MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS Cam Mobile Extension: camera do iPhone pelo cabo";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Cam Mobile Extension";
}

bool obs_module_load(void)
{
	// O core e compilado contra os headers do FFmpeg e roda com o FFmpeg que o OBS carrega: o ABI so vale com a
	// mesma versao maior. Se o OBS trocar de FFmpeg, o modulo se recusa em vez de travar a transmissao.
	unsigned v = avcodec_version();
	if ((v >> 16) != LIBAVCODEC_VERSION_MAJOR) {
		blog(LOG_ERROR, "[fmcam] libavcodec do OBS (%u.%u) diferente da usada na compilacao (%d): recompile a extensao",
		     v >> 16, (v >> 8) & 0xff, LIBAVCODEC_VERSION_MAJOR);
		return false;
	}
	std::string caminho = fmcam::caminhoUsbmuxConfigurado();
	// Bonjour: FMCAM_BONJOUR=off desliga, =local so enxerga anuncios da propria maquina (testes, sem tocar a rede)
	const char *bj = getenv("FMCAM_BONJOUR");
	std::unique_ptr<fmcam::DescobertaWifi> wifi;
	if (!bj || std::string(bj) != "off")
		wifi = fmcam::criarDescobertaWifi(bj && std::string(bj) == "local" ? 0xFFFFFFFFu : 0u);
	fmcam::g_registro = std::make_unique<fmcam::RegistroAparelhos>(caminho, std::move(wifi));
	fmcam::g_assinatura = fmcam::g_registro->assinar([] { fmcam::recarregarPropriedades(); });
	fmcam::g_registro->iniciar();
	fmcam::registrarFonte();
#ifdef FMCAM_COM_PAINEL
	fmcam::iniciarPainel();
#endif
	blog(LOG_INFO, "[fmcam] OBS Cam Mobile Extension carregada (usbmuxd: %s)", caminho.c_str());
	return true;
}

void obs_module_unload(void)
{
#ifdef FMCAM_COM_PAINEL
	fmcam::pararPainel();
#endif
	if (fmcam::g_registro) {
		fmcam::g_registro->desassinar(fmcam::g_assinatura);
		fmcam::g_registro->parar();
	}
	// Sessoes que ainda estao fechando em segundo plano usam o registro: espera (com limite) antes de soltar.
	if (!fmcam::aguardarEncerramentos(3000))
		blog(LOG_WARNING, "[fmcam] %d sessao(oes) ainda encerrando ao descarregar", fmcam::sessoesEncerrando());
	fmcam::g_registro.reset();
}
