# OBS Cam Mobile Extension para Windows x64, compilada de outro sistema (Mac) com clang-cl.
# Projeto separado de proposito: o obs-plugin/CMakeLists.txt do Mac nao e tocado e o core/ nao pede pkg-config aqui.
# Fontes e definicoes iguais as do modulo do obs-plugin/CMakeLists.txt (ramo WIN32).
cmake_minimum_required(VERSION 3.20)
project(fmcam_extension_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RAIZ "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PLUGIN "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CORE "${RAIZ}/core")

set(FMCAM_OBS_SRC "${PLUGIN}/deps/obs-studio" CACHE PATH "headers do obs-studio (mesma tag do OBS alvo)")
set(FMCAM_SIMDE_INC "${PLUGIN}/deps/simde" CACHE PATH "headers do simde")
set(FMCAM_OBS_LIB_DIR "" CACHE PATH "obs.lib e obs-frontend-api.lib (geradas das DLLs do OBS)")
set(FMCAM_DEPS_DIR "" CACHE PATH "obs-deps do Windows (include/ e lib/ do FFmpeg)")
set(FMCAM_QT_DIR "" CACHE PATH "Qt6 do obs-deps do Windows (include/ e lib/)")
option(FMCAM_COM_PAINEL "Painel (dock) Qt6" ON)
set(FMCAM_CRT "MultiThreaded" CACHE STRING "MultiThreaded (/MT estatico) ou MultiThreadedDLL (/MD)")
set(CMAKE_MSVC_RUNTIME_LIBRARY "${FMCAM_CRT}")

set(LOGICA
  ${PLUGIN}/src/logica/aparelhos.cc
  ${PLUGIN}/src/logica/ajuste_itens.cc
  ${PLUGIN}/src/logica/config_fonte.cc
  ${PLUGIN}/src/logica/modelo_iphone.cc
  ${PLUGIN}/src/logica/quadro.cc
  ${PLUGIN}/src/logica/sessao.cc
  ${PLUGIN}/src/logica/descoberta_wifi_win.cc)
set(FONTES ${PLUGIN}/src/glue/modulo.cc ${PLUGIN}/src/glue/fonte.cc ${LOGICA}
  ${CORE}/rede.cc ${CORE}/protocolo.cc ${CORE}/usbmux.cc ${CORE}/conexao.cc ${CORE}/decodificador.cc)
if(FMCAM_COM_PAINEL)
  list(APPEND FONTES ${PLUGIN}/src/painel/painel.cc)
endif()

add_library(fmcam-extension MODULE ${FONTES})
set_target_properties(fmcam-extension PROPERTIES PREFIX "")
target_include_directories(fmcam-extension PRIVATE
  ${PLUGIN}/src/logica ${PLUGIN}/src/glue ${CORE}
  ${FMCAM_SIMDE_INC} ${PLUGIN}/cmake/obsconfig ${FMCAM_OBS_SRC}/libobs ${FMCAM_OBS_SRC}/frontend/api
  ${FMCAM_DEPS_DIR}/include)
target_compile_definitions(fmcam-extension PRIVATE
  QT_NO_DEBUG QT_NO_DEBUG_OUTPUT WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
target_compile_options(fmcam-extension PRIVATE /W3 -Wno-unused-parameter
  /FI${CMAKE_CURRENT_SOURCE_DIR}/compat-clang-cl.h)

target_link_libraries(fmcam-extension PRIVATE
  ${FMCAM_OBS_LIB_DIR}/obs.lib
  ${FMCAM_DEPS_DIR}/lib/avcodec.lib ${FMCAM_DEPS_DIR}/lib/avutil.lib
  ws2_32 dnsapi)

if(FMCAM_COM_PAINEL)
  target_compile_definitions(fmcam-extension PRIVATE FMCAM_COM_PAINEL=1)
  target_include_directories(fmcam-extension PRIVATE ${PLUGIN}/src/painel
    ${FMCAM_QT_DIR}/include ${FMCAM_QT_DIR}/include/QtCore ${FMCAM_QT_DIR}/include/QtGui ${FMCAM_QT_DIR}/include/QtWidgets)
  target_link_libraries(fmcam-extension PRIVATE ${FMCAM_OBS_LIB_DIR}/obs-frontend-api.lib
    ${FMCAM_QT_DIR}/lib/Qt6Core.lib ${FMCAM_QT_DIR}/lib/Qt6Gui.lib ${FMCAM_QT_DIR}/lib/Qt6Widgets.lib)
endif()

# Estrutura que o OBS do Windows carrega: <plugins>/fmcam-extension/{bin/64bit,data/locale}
install(TARGETS fmcam-extension RUNTIME DESTINATION fmcam-extension/bin/64bit LIBRARY DESTINATION fmcam-extension/bin/64bit)
install(FILES ${PLUGIN}/data/locale/en-US.ini ${PLUGIN}/data/locale/pt-BR.ini DESTINATION fmcam-extension/data/locale)
if(FMCAM_COM_PAINEL)
  install(FILES ${RAIZ}/marca/icone-pb-32.png ${RAIZ}/marca/icone-pb-64.png DESTINATION fmcam-extension/data)
endif()
