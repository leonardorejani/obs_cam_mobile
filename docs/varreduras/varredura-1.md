# Varredura 1 (24/11/2026)

Versão entregue: app **1.0.3 build 8** e extensão **1.0.3**. Aparelho usado: só o iPhone 15 Pro Max do Leonardo, no cabo, iOS 27.2. Computador: Mac M5 com OBS 33.0.1. Nada foi aberto na tela do Mac: o OBS foi medido com o libobs de verdade sem janela (`obs-plugin/tests/medir_obs.cc`) e o iPhone foi conferido pelo log (`idevicesyslog`) e por fotos tiradas pelo próprio app (`FMCAM_DEBUG=1`). Provas em `medicoes/varredura1/`.

## 4K: números antes e depois

Como foi medido: o relógio dos dois lados é sincronizado por ping (`PROTOCOLO.md` seção 3). A extensão agora mede o atraso **no instante em que entrega o quadro decodificado ao OBS** (captura até OBS) e o `medir_obs` mede quanto o libobs leva até terminar de desenhar o canvas com aquele quadro (OBS até canvas, fonte sem buffer). Cabo, iPhone em pé (por isso 1080x1920 e 2160x3840), canvas 1920x1080, mediana de 12 a 20 s.

| Formato | Antes: captura até chegada / até decodificado (visor, app 1.0.2) | Antes: captura até OBS / até canvas (libobs) | Depois: captura até OBS / até canvas (app e extensão 1.0.3) |
|---|---|---|---|
| 720p30 | - | 45 ms / 58 ms | 45 ms / 73 ms |
| 1080p30 | 49 ms / 51 ms | - (a rodada saiu em 4K, inválida) | 54 ms / 59 ms |
| 1080p60 | 55 ms / 57 ms | 51 ms / 63 ms | 53 ms / 63 ms |
| 2160p30, 10 Mbps | 76 ms / 81 ms | - | - |
| 2160p30, 25 Mbps | 77 ms / 84 ms | 82 ms / 91 ms | 81 ms / 106 ms |
| 2160p30, 25 Mbps, canvas 3840x2160 | - | 69 ms / 86 ms | 81 ms / 105 ms |
| 2160p30, decodificação só por software | - | 87 ms / 97 ms | - |
| 1080p30, Mais estável | - | - | 170 ms (p95 173 ms), como esperado: menor atraso + 120 ms |

O "OBS até canvas" varia de 6 a 28 ms de uma rodada para outra porque depende de em que ponto do ciclo de 33 ms o quadro chega (o OBS desenha 30 vezes por segundo). O que vale comparar é o "captura até OBS".

Conclusões:

- **Com o iPhone já transmitindo, o 4K está abaixo de ~100 ms no cabo** (cerca de 81 ms até o OBS). A diferença para o 1080p (cerca de 27 ms) está toda no iPhone: codificar um quadro 4 vezes maior. O bitrate não muda nada (10 e 25 Mbps deram 76 e 77 ms), a fila do iPhone ficou em 0 bytes e a decodificação por hardware no Mac leva 5 a 8 ms. A extensão e o OBS não seguram quadro em 4K.
- **O atraso sem sentido que dava para ver no 4K era a partida**, não o vídeo andando. No log do OBS do Leonardo de hoje de manhã (`obs-log-usuario-2026-11-24-manha.txt`) a fonte em 4K cai e volta a cada 2 a 5 s ("iPhone fechou a conexao", "3 s sem dados", "iPhone nao respondeu ao handshake"), segurando o último quadro a cada queda. No aparelho (`sys-4k-repro.txt`) o app ligava a câmera duas vezes ao abrir, sempre em 1080p, e a primeira conexão em 4K reconfigurava a câmera no meio: "câmera pronta em 2386 ms" e "2446 ms", colado no limite de 2,5 s, e às vezes passava (erro "camera" no cliente). Depois da correção: uma partida só, já no formato salvo, **"câmera pronta em 674 ms"** (`depois/syslog-noar.txt`).
- Num PC lento sem decodificação por hardware, o teste `decodificador_lento_nao_deixa_o_atraso_crescer_ate_o_limite_da_fila_do_iphone` mostra o que acontece com um decodificador de 50 ms por quadro: o atraso sobe até cerca de 390 ms e quadros são pulados. Para o operador ver isso agora existe o status ao vivo (mostra "decodificação por hardware" e o atraso) e o botão Testar (avisa "o computador pulou quadros"), e a opção Decodificação permite forçar o hardware.

## Bugs achados

| # | Onde | Sintoma | Causa raiz | Correção | Teste ou prova | Status |
|---|---|---|---|---|---|---|
| 1 | App, `Captura.swift` `definirAjustesIniciais` | Com 4K salvo, a primeira conexão demorava 2,4 s e às vezes caía com erro "camera" | A câmera abria sempre em 1080p30 e a primeira conexão em 4K reconfigurava a sessão dentro do limite de partida | Câmera abre no formato salvo | `testCameraAbreNoFormatoSalvo` (`ios-camera-abre-no-formato-salvo.txt`), aparelho: 2386/2446 ms antes, 674 ms depois | Corrigido |
| 2 | App, `Captura.swift` `ligarEmPrimeiroPlano` | Câmera ligada duas vezes ao abrir o app, com um instante na ultra-angular | `onAppear` e `scenePhase .active` chamavam os dois | Segunda chamada com a câmera de pé não mexe nela | `testSegundaChamadaComACameraDePeNaoMexeNela`, aparelho: uma linha "primeiro plano" só e sem "ultra" na abertura | Corrigido |
| 3 | App, `Tela.swift` (em pé) | Com NO AR e o iPhone em pé, a borda vermelha riscava o NO AR, o estado e o nome na faixa de cima | A borda era desenhada por cima dos controles e a faixa de cima fica sobre a borda de cima do vídeo | Borda desenhada atrás dos controles | Fotos do app `ios-borda-no-ar-em-pe-antes.png` e `-depois.png`. Paisagem conferida (`depois/noar-2.png`, `noar-3.png`) | Corrigido |
| 4 | App, `Servidor.swift` `qualidadeSalvaMudou` | Trocar a qualidade e voltar à antiga com a troca em andamento: câmera no formato novo, tela mostrando o antigo | Comparava com a qualidade aplicada, não com a última pedida | Guarda a qualidade alvo | `testVoltarAQualidadeAntigaComTrocaEmAndamentoPedeDeNovo` | Corrigido |
| 5 | App, `Captura.swift` `FonteCamera.reconfigurar` | Mesmo cenário do 4 na camada da câmera | O formato só mudava no fim da troca | Vale na hora do pedido, falha restaura | `testFonteVoltaAoFormatoAntigoComATrocaAnteriorEmAndamento` | Corrigido |
| 6 | App, `Servidor.swift` `aoIniciarFonte` | Qualidade mudada no iPhone enquanto a câmera ligava se perdia | Aviso ignorado na fase de partida | Reconcilia quando a câmera fica pronta | `testQualidadeMudadaDuranteAPartidaValeQuandoACameraFicaPronta` | Corrigido |
| 7 | App, `Captura.swift` `tratarErroDeExecucao` | Segundo erro de execução da câmera na mesma vida do app derrubava a conexão e deixava a câmera parada | A religada automática só acontecia uma vez por vida | Uma religada a cada 30 s | `testPoliticaDeReinicioVoltaATentarDepoisDeUmTempo` (prova a regra, a religada real só no aparelho) | Corrigido |
| 8 | App, `Tela.swift` `alternarGrupo` e `alternarLimpo` | Nome da câmera digitado se perdia ao tocar em outro item do menu ou no limpar | Só fechar o painel ou tocar na imagem salvava | Salva antes de fechar o painel | `testNomeDigitadoFicaSalvoAoTrocarDeGrupo`, `...AoLimparATela` | Corrigido |
| 9 | App, `Tela.swift` `definirTrava` | "AE/AF travado" do toque longo ficava na tela depois de destravar | Só outro toque limpava | Some ao desligar a trava | `testMarcaDeTravadoSomeQuandoAsTravasSaem` | Corrigido |
| 10 | App, painel Qualidade | Formato salvo que a lente não tem aparecia selecionado e desabilitado ao mesmo tempo | Mostrava o salvo, não o que sai | Mostra o mais próximo abaixo, que é o que sai | `testPainelMostraOFormatoQueDeFatoSai` | Corrigido |
| 11 | Núcleo, `core/protocolo.cc` `lerPedidoHandshake` | Quem lê o handshake em C++ (iPhone falso dos testes) trocava `2160p30` e `1080p120` por `1080p30`, então o caminho 4K nunca foi testado de ponta a ponta | Lista de presets aceitos incompleta | Aceita os 6 presets da extensão | `handshake_aceita_todos_os_presets_da_extensao` (`core-preset-4k-antes.txt` falha, `-depois.txt` passa) | Corrigido |
| 12 | Extensão Windows, `descoberta_wifi_win.cc` | Fonte removida ou OBS fechando com uma resolução de Wi-Fi em andamento: callback do Windows usando objeto já destruído (queda do OBS) | Contextos com ponteiro cru, resoluções pendentes nunca canceladas | Estado dos callbacks em `shared_ptr` com flag `vivo` | Compilação cruzada. Não há Windows para rodar | Corrigido sem execução no Windows |
| 13 | Núcleo, `core/conexao.cc` (vigia do cabo e thread solta no `parar`) | No Windows, fechar o OBS com uma dessas threads viva executa código da DLL já descarregada (queda ao fechar) | Threads soltas (`detach`) não eram esperadas no `obs_module_unload` | Contador de threads soltas e espera de até 1 s ao descarregar | Leitura do código e compilação. Não dá para reproduzir no Mac | Corrigido sem execução no Windows |
| 14 | Extensão, propriedades | Versão do cliente no handshake e no bundle diziam 1.0.0 | Valor fixo antigo | 1.0.3 | `opcoes_novas_normalizam...` confere o nome do cliente | Corrigido |

### Achados não corrigidos

| Onde | O que | Por que não |
|---|---|---|
| Fonte em cena que não aparece | Continua conectada e decodificando: 4K oculto gastou 89 ms de CPU por segundo no M5 (`depois/obs-2160p30-oculta.txt`), igual ao visível. A banda do cabo também continua | Pausar a decodificação faz a fonte mostrar por um instante o quadro velho ao entrar no ar sem modo estúdio, e pausar o iPhone obrigaria religar a câmera. Os dois pioram a live. Fica a decisão para o Leonardo |
| iPhone em 4K | 27 ms a mais que o 1080p, todos no iPhone (codificação) | O codificador já está no modo de menor atraso (`MaxFrameDelayCount` 0, velocidade antes de qualidade). Não achei mais o que tirar sem trocar de codec |
| Propriedades da fonte | O status ao vivo recarrega a janela de propriedades a cada 2 s quando um valor muda. Se o operador estiver digitando o IP do Wi-Fi manual nessa hora, o campo perde o foco (o texto fica) | O OBS só atualiza texto de propriedade recarregando tudo. Só recarrega com valor visível diferente (atraso de 5 em 5 ms, bateria em %) |
| Windows, `usbmux.cc` | Se algum serviço USB da Apple não mandar `ConnectionType`, o iPhone some do cabo | Não provado: as versões atuais mandam |
| Windows, instalador | `msiexec` 1638 (serviço da Apple já instalado em outra versão) aborta o instalador | Não provado qual código o MSI da Apple devolve |
| Windows, Wi-Fi | Anúncio com TTL 0, interface 0 e resolução que falha não é tentada de novo | Só dá para validar no PC da transmissão. Se um iPhone sumir da lista Wi-Fi até reiniciar o OBS, é aqui |
| App, interrupção da câmera | Ligação ou outro app usando a câmera: a tela continua "Conectado" com a imagem parada até o watchdog derrubar (1,5 s) | Precisa de teste com ligação real |
| App, modo limpo | O ponto amarelo da validade (menos de 1 dia) existe na lógica mas nenhuma tela usa | Comportamento a decidir com o Leonardo |
| App, toque longo em foco manual | Acende "Trava AF" com o foco em manual, e ao voltar para Auto o foco fica travado | Precisa de aparelho para confirmar o efeito |
| App, térmico e bateria | Em "serious" e "critical" só o chip muda, nenhuma ação automática | Não é defeito, é decisão de produto |

Medido também e sem defeito: troca de lente pelos 4 botões (zoom efetivo 10, 1 e 2 no dispositivo virtual), travas AE, AF e BB e EV mudam o modo efetivo do `AVCaptureDevice` (log "fmcam estado": exposição 0/2, foco 0/2, balanço 0/2, bias -1/0).

## Funções novas da extensão (1.0.3)

Todas são acréscimo: o padrão é o comportamento de antes, exceto o Encaixar, que nasce ligado só em fonte nova.

1. **Status ao vivo nas propriedades da fonte:** canal (Cabo, Wi-Fi ou "cabo pela entrada de rede"), formato e se a decodificação é por hardware, atraso do iPhone até o OBS (p50 e p95 dos últimos 3 s), quadros por segundo, bitrate (configurado e medido), bateria e temperatura.
2. **Encaixar na tela sozinho** (caixa de seleção). Regra: quando o tamanho do vídeo muda (girou o iPhone ou trocou a resolução, inclusive com o OBS fechado, porque o último tamanho fica salvo na fonte), item da cena **sem** caixa delimitadora, sem recorte e sem espelho passa a ter caixa delimitadora do retângulo que já ocupava, com a mesma posição, alinhamento e rotação, e o vídeo novo cabe dentro. Item com caixa delimitadora o OBS já mantém e não é tocado. Item recortado ou espelhado não é tocado. A regra antiga (item que enche o canvas) continua igual. Fonte nova nasce com ela ligada (inclusive as criadas pelo painel). Fonte criada antes da 1.0.3 fica desligada até o operador marcar.
3. **Modo de recepção:** Menor atraso (padrão) ou Mais estável (cada quadro sai no instante captura + menor atraso visto nos últimos 2 s + 120 ms. Medido: 170 ms com p95 de 173 ms).
4. **Quando o iPhone cair:** Manter o último quadro (padrão), Tela preta (preto no último tamanho do vídeo) ou Imagem (PNG, JPG, BMP ou GIF parado, decodificada ao salvar). Vale sem reconectar.
5. **Decodificação:** Automática (padrão), Hardware ou Software.
6. **Painel OBS Cam Mobile com controle remoto** por iPhone: botões de lente, EV (menos e mais de 0,3), Trava AE, Trava AF, Trava BB e Qualidade (resolução e quadros, bitrate, Aplicar). Os comandos novos do protocolo estão em `PROTOCOLO.md`. Conferido de ponta a ponta no 15 Pro Max (`remoto/medir.txt`, `remoto/syslog.txt`, fotos `remoto/snap-*.png`): os 12 comandos chegaram, o iPhone aplicou e o status voltou com o valor novo em menos de 3 s. O painel não reconstrói mais os widgets a cada segundo (antes ele apagava e recriava tudo a cada mudança de fps ou bateria), senão um menu aberto fecharia sozinho.
7. **Testar** por iPhone no painel: mede 3 s e diz "Cabo ok", "Funciona, com ressalvas" ou "Atraso alto", com atraso, ida e volta, fps e os avisos (critérios no `GUIA-OPERADOR.md`).

Textos novos em pt-BR e en-US. A fonte ganhou o proc `fmcam_comando` (para automação e medição) e o `fmcam_estado` passou a dizer atraso, ida e volta, canal, hardware e o estado do controle remoto.

## Testes

- `COMPLETO=1 sh .claude/verificar.sh` verde (`medicoes/varredura1/verificar-completo-final.txt`): ctest 14 de 14 (core 35 casos, lógica da fonte 37 casos, headless do OBS com os checks novos) e iOS 248 testes, 0 falhas (eram 213).
- Novos no ctest: `handshake_aceita_todos_os_presets_da_extensao` (core) e em `obs_logica_fonte`: `controle_remoto_do_painel_chega_ao_iphone_e_status_novo_e_lido`, `status_de_app_antigo_nao_liga_os_controles_remotos`, `botao_testar_avalia_cabo_atraso_ida_e_volta_fps_e_descartes`, `encaixar_mantem_o_retangulo_do_item_e_nao_toca_bounds_recorte_nem_espelho`, `opcoes_novas_normalizam_e_so_decodificacao_e_recepcao_reconectam`, `encaixar_nasce_ligado_so_em_fonte_nova`, `decodificacao_hardware_ou_software_escolhida_na_fonte`, `recepcao_mais_estavel_segura_a_reserva_e_menor_atraso_nao`. No `obs_headless` (libobs de verdade): PIP mantém 480x270 em 1400,780 quando o iPhone gira, e a queda mostra tela preta.
- Novos no iOS: `VarreduraTests.swift` e `ControleRemotoTests.swift`.

## Onde verificar

Feche e abra o OBS uma vez para carregar a extensão 1.0.3. iPhone no cabo, app aberto.

1. **4K sem demora na partida:** no iPhone, Ajustes > Qualidade > 2160p 30. Feche o app (deslize para cima) e abra de novo. No OBS a imagem tem que voltar em 1 a 2 s, sem ficar caindo e voltando, e sem a imagem passar pela ultra-angular.
2. **Status ao vivo:** clique duas vezes na fonte. Abaixo do estado aparecem Canal, Formato, Atraso do iPhone até o OBS, Quadros por segundo, Bitrate, Bateria e Temperatura. Deixe a janela aberta: os números mudam sozinhos a cada 2 s. Em 4K o atraso deve ficar perto de 80 ms, em 1080p perto de 55 ms.
3. **Encaixar:** fonte nova (Fontes > + ou Adicionar à cena no painel). Diminua o item para um canto (picture-in-picture). Gire o iPhone de deitado para em pé: o item fica no mesmo lugar e do mesmo tamanho, com o vídeo em pé dentro (faixas laterais). Numa fonte antiga, marque **Encaixar na tela sozinho** nas propriedades para ter o mesmo.
4. **Quando o iPhone cair:** nas propriedades, Tela preta. Feche o app no iPhone: a fonte fica preta. Abra o app: o vídeo volta. Depois Imagem, escolha um PNG e repita. Manter o último quadro é o de sempre.
5. **Modo de recepção:** Mais estável. A fonte reconecta e o atraso no status sobe cerca de 120 ms. Volte para Menor atraso.
6. **Decodificação:** Software, e o status perde o "(decodificação por hardware)" em 4K. Volte para Automática.
7. **Painel (Docks > OBS Cam Mobile):** toque em 5x, 0,5x e 1x: a lente muda no iPhone e o botão aceso muda nos dois. EV mais e menos: a régua de EV do iPhone anda. Trava AE, Trava AF e Trava BB: os botões do trilho Câmera do iPhone acendem e apagam junto. Qualidade: escolha 1080p30 e 15 Mbps, Aplicar: o iPhone troca sem reconectar e Ajustes > Qualidade mostra o novo. Com uma predefinição fixada nas propriedades da fonte, o combo de resolução fica travado no painel.
8. **Testar:** no painel, Testar. Depois de 3 s aparece "Cabo ok. atraso ... ms (p95 ... ms), ida e volta ... ms, 30/30 fps." Com o iPhone pelo Wi-Fi aparece a ressalva "não está pelo cabo".
9. **NO AR em pé:** ponha a cena da fonte no Programa com o iPhone em pé. A borda vermelha passa atrás da faixa de cima: NO AR, Conectado e o nome ficam legíveis.
10. **Nome da câmera:** Ajustes > Nome, digite outro nome e toque direto em Guias. Volte em Ajustes: o nome ficou salvo.
11. **Marca de travado:** toque longo na imagem (aparece "AE/AF travado"), depois desligue Trava AE e Trava AF no trilho: a marca some.
12. **Windows:** instalar o `OBSCamMobile-Extension-Setup.exe` 1.0.3 com o OBS fechado e repetir 2, 7 e 8 no PC da transmissão. Fechar o OBS com o iPhone conectado não pode dar erro.
