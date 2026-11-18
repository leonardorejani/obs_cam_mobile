# OBS Cam Mobile Extension: fonte do OBS em cima do core/. Estrutura inspirada no obs-plugintemplate oficial (GPLv2),
# sem copiar codigo dele. Mac (ad-hoc) e Windows (MSVC) saem do mesmo arquivo.
cmake_minimum_required(VERSION 3.20)

set(FMCAM_OBS_APP "/Applications/OBS.app" CACHE PATH "Mac: OBS.app de onde vem libobs, FFmpeg e Qt")
set(FMCAM_OBS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-studio" CACHE PATH "Codigo-fonte do obs-studio na MESMA versao do OBS instalado (so os headers sao usados)")
set(FMCAM_OBS_TAG "33.0.1" CACHE STRING "Tag do obs-studio baixada se FMCAM_OBS_SRC nao existir")
# Windows: onde estao libobs.lib, obs-frontend-api.lib e as libs/headers do FFmpeg do obs-deps
set(FMCAM_OBS_LIB_DIR "" CACHE PATH "Windows: pasta com obs.lib e obs-frontend-api.lib")
set(FMCAM_FFMPEG_INCLUDE_DIR "" CACHE PATH "Headers do FFmpeg da mesma versao maior que o OBS carrega (obs-deps no Windows)")
set(FMCAM_FFMPEG_LIB_DIR "" CACHE PATH "Windows: pasta com avcodec.lib e avutil.lib")
set(FMCAM_QT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/qt/6.11.1/macos" CACHE PATH "Mac: Qt6 (so headers) da MESMA versao que o OBS carrega (aqt install-qt mac desktop 6.11.1 clang_64)")
set(FMCAM_QT_LIB_DIR "" CACHE PATH "Windows: pasta do Qt6 do OBS (obs-deps) com Qt6Widgets.lib")
if(APPLE AND EXISTS "${FMCAM_QT_ROOT}/lib/QtWidgets.framework/Headers")
  option(FMCAM_COM_PAINEL "Painel (dock) Qt6" ON)
else()
  option(FMCAM_COM_PAINEL "Painel (dock) Qt6" OFF)
endif()

if(NOT EXISTS "${FMCAM_OBS_SRC}/libobs/obs.h")
  find_package(Git REQUIRED)
  message(STATUS "Baixando headers do obs-studio ${FMCAM_OBS_TAG}")
  execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch ${FMCAM_OBS_TAG}
                          https://github.com/obsproject/obs-studio.git "${FMCAM_OBS_SRC}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nao consegui baixar o obs-studio ${FMCAM_OBS_TAG}; ajuste FMCAM_OBS_SRC")
  endif()
endif()

# libobs inclui simde (so headers, MIT). No Mac vem do Homebrew ou de deps/.
set(FMCAM_SIMDE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/deps/simde" CACHE PATH "Headers do simde")
find_path(SIMDE_INC simde/x86/sse2.h HINTS "${FMCAM_SIMDE_SRC}" /opt/homebrew/include /usr/local/include)
if(NOT SIMDE_INC)
  find_package(Git REQUIRED)
  execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch v0.8.2
                          https://github.com/simd-everywhere/simde.git "${FMCAM_SIMDE_SRC}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "simde nao encontrado; ajuste FMCAM_SIMDE_SRC")
  endif()
  set(SIMDE_INC "${FMCAM_SIMDE_SRC}")
endif()
set(OBS_INCLUDES ${SIMDE_INC} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/obsconfig ${FMCAM_OBS_SRC}/libobs ${FMCAM_OBS_SRC}/frontend/api)

# ---- FFmpeg: headers + libs que o OBS carrega ----
if(APPLE)
  set(OBS_FW "${FMCAM_OBS_APP}/Contents/Frameworks")
  if(NOT EXISTS "${OBS_FW}/libobs.framework/Versions/A/libobs")
    message(FATAL_ERROR "OBS nao encontrado em ${FMCAM_OBS_APP}")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFH REQUIRED libavcodec libavutil)  # so os headers: a lib de execucao e a do OBS
  set(FF_INC ${FFH_INCLUDE_DIRS})
  set(FF_LIBS "${OBS_FW}/libavcodec.dylib" "${OBS_FW}/libavutil.dylib")
  set(OBS_LIBS "${OBS_FW}/libobs.framework/Versions/A/libobs")
  set(OBS_FRONTEND_LIBS "${OBS_FW}/obs-frontend-api.dylib")
else()
  set(FF_INC "${FMCAM_FFMPEG_INCLUDE_DIR}")
  find_library(FF_AVCODEC avcodec HINTS "${FMCAM_FFMPEG_LIB_DIR}" REQUIRED)
  find_library(FF_AVUTIL avutil HINTS "${FMCAM_FFMPEG_LIB_DIR}" REQUIRED)
  set(FF_LIBS ${FF_AVCODEC} ${FF_AVUTIL})
  find_library(OBS_LIB NAMES obs libobs HINTS "${FMCAM_OBS_LIB_DIR}" REQUIRED)
  find_library(OBS_FRONTEND_LIB NAMES obs-frontend-api HINTS "${FMCAM_OBS_LIB_DIR}" REQUIRED)
  set(OBS_LIBS ${OBS_LIB})
  set(OBS_FRONTEND_LIBS ${OBS_FRONTEND_LIB})
endif()

find_package(Threads REQUIRED)
set(CORE ${CMAKE_SOURCE_DIR}/core)
set(LOGICA
  src/logica/aparelhos.cc
  src/logica/ajuste_itens.cc
  src/logica/config_fonte.cc
  src/logica/modelo_iphone.cc
  src/logica/quadro.cc
  src/logica/sessao.cc)
if(APPLE)
  list(APPEND LOGICA src/logica/descoberta_wifi_mac.cc)
elseif(WIN32)
  list(APPEND LOGICA src/logica/descoberta_wifi_win.cc)
else()
  message(FATAL_ERROR "plataforma sem descoberta Bonjour")
endif()

# ---- logica sem libobs, ligada ao core normal (testes do ctest) ----
add_library(fmcam_logica STATIC ${LOGICA})
target_include_directories(fmcam_logica PUBLIC src/logica)
target_link_libraries(fmcam_logica PUBLIC fmappcam_core)
if(WIN32)
  target_link_libraries(fmcam_logica PUBLIC dnsapi)
endif()

add_executable(teste_logica_fonte tests/teste_logica_fonte.cc)
target_include_directories(teste_logica_fonte PRIVATE ${CMAKE_SOURCE_DIR}/tools tests)
target_link_libraries(teste_logica_fonte PRIVATE fmcam_logica fmappcam_teste)

if(APPLE)
  add_test(NAME obs_logica_fonte COMMAND teste_logica_fonte)
  set_tests_properties(obs_logica_fonte PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT "FMCAM_IPHONE_FALSO=$<TARGET_FILE:iphone-falso>;FMCAM_AMOSTRA=${CMAKE_BINARY_DIR}/amostra.h264")
endif()

# ---- o modulo do OBS ----
set(MODULO_FONTES src/glue/modulo.cc src/glue/fonte.cc ${LOGICA}
  ${CORE}/rede.cc ${CORE}/protocolo.cc ${CORE}/usbmux.cc ${CORE}/conexao.cc ${CORE}/decodificador.cc)
set(LOCALES data/locale/en-US.ini data/locale/pt-BR.ini)

if(FMCAM_COM_PAINEL)
  list(APPEND MODULO_FONTES src/painel/painel.cc)
  if(APPLE)
    foreach(i 32 64)
      set(ICONE ${CMAKE_SOURCE_DIR}/marca/icone-pb-${i}.png)
      set_source_files_properties(${ICONE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
      list(APPEND MODULO_FONTES ${ICONE})
    endforeach()
  endif()
endif()

if(APPLE)
  foreach(l ${LOCALES})
    set_source_files_properties(${l} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/locale)
  endforeach()
  list(APPEND MODULO_FONTES ${LOCALES})
endif()

add_library(fmcam-extension MODULE ${MODULO_FONTES})
target_include_directories(fmcam-extension PRIVATE
  src/logica src/glue ${CORE}
  ${OBS_INCLUDES}
  ${FF_INC})
target_compile_features(fmcam-extension PRIVATE cxx_std_17)
target_link_libraries(fmcam-extension PRIVATE ${OBS_LIBS} ${FF_LIBS} Threads::Threads)
if(FMCAM_COM_PAINEL)
  target_compile_definitions(fmcam-extension PRIVATE FMCAM_COM_PAINEL=1)
  target_include_directories(fmcam-extension PRIVATE src/painel)
  target_link_libraries(fmcam-extension PRIVATE ${OBS_FRONTEND_LIBS})
  if(APPLE)
    # headers do Qt vindos do aqt (mesma versao do OBS); a biblioteca de execucao e a do OBS
    # No Mac o Qt so tem headers dentro dos frameworks: monta uma arvore <QtCore/...> por links simbolicos
    set(QT_INC "${CMAKE_CURRENT_BINARY_DIR}/qt-include")
    foreach(m QtCore QtGui QtWidgets)
      file(MAKE_DIRECTORY "${QT_INC}")
      file(CREATE_LINK "${FMCAM_QT_ROOT}/lib/${m}.framework/Headers" "${QT_INC}/${m}" SYMBOLIC)
    endforeach()
    target_include_directories(fmcam-extension BEFORE PRIVATE ${QT_INC})
    target_link_libraries(fmcam-extension PRIVATE
      "${OBS_FW}/QtWidgets.framework/Versions/A/QtWidgets"
      "${OBS_FW}/QtGui.framework/Versions/A/QtGui"
      "${OBS_FW}/QtCore.framework/Versions/A/QtCore")
  else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets HINTS "${FMCAM_QT_LIB_DIR}")
    target_link_libraries(fmcam-extension PRIVATE Qt6::Widgets)
  endif()
endif()

if(WIN32)
  target_link_libraries(fmcam-extension PRIVATE ws2_32 dnsapi)
  set_target_properties(fmcam-extension PROPERTIES PREFIX "")
  # Windows: instala em %ProgramData%\obs-studio\plugins\fmcam-extension\bin\64bit + data\locale
  install(TARGETS fmcam-extension RUNTIME DESTINATION fmcam-extension/bin/64bit LIBRARY DESTINATION fmcam-extension/bin/64bit)
  install(FILES ${LOCALES} DESTINATION fmcam-extension/data/locale)
elseif(APPLE)
  set_target_properties(fmcam-extension PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@executable_path/../Frameworks"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
  # Assinatura ad-hoc do bundle (o OBS carrega com disable-library-validation)
  add_custom_command(TARGET fmcam-extension POST_BUILD
    COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:fmcam-extension>
    COMMENT "codesign ad-hoc")
  add_custom_target(instalar-obs
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/instalar-mac.sh $<TARGET_BUNDLE_DIR:fmcam-extension>
    DEPENDS fmcam-extension
    COMMENT "Instalando em ~/Library/Application Support/obs-studio/plugins")
endif()

# ---- testes que carregam o modulo (so Mac, sem janela) ----
if(APPLE)
  # core sem FFmpeg (rede, protocolo, usbmux) para os executaveis que tambem ligam o libobs
  add_library(fmcam_core_rede STATIC ${CORE}/rede.cc ${CORE}/protocolo.cc ${CORE}/usbmux.cc)
  target_include_directories(fmcam_core_rede PUBLIC ${CORE})

  add_executable(teste_carga_modulo tests/teste_carga_modulo.cc)
  add_executable(teste_headless_obs tests/teste_headless_obs.cc)
  target_include_directories(teste_headless_obs PRIVATE ${CMAKE_SOURCE_DIR}/tools tests ${OBS_INCLUDES})
  target_include_directories(teste_carga_modulo PRIVATE ${OBS_INCLUDES})
  target_link_libraries(teste_headless_obs PRIVATE fmcam_core_rede ${OBS_LIBS} Threads::Threads)
  foreach(t teste_carga_modulo teste_headless_obs)
    set_target_properties(${t} PROPERTIES BUILD_RPATH "${OBS_FW}")
  endforeach()
  target_link_libraries(teste_carga_modulo PRIVATE ${OBS_LIBS})

  set(PLUGIN_INSTALADO "$ENV{HOME}/Library/Application Support/obs-studio/plugins/fmcam-extension.plugin")
  add_test(NAME obs_carga_modulo COMMAND teste_carga_modulo "${PLUGIN_INSTALADO}")
  # amostra 720p para provar a troca de resolucao (precisa do ffmpeg na linha de comando)
  set(AMOSTRA720 ${CMAKE_BINARY_DIR}/amostra720.h264)
  add_custom_command(OUTPUT ${AMOSTRA720}
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/amostra/gerar.sh ${AMOSTRA720} 8 1280x720
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/amostra/gerar.sh COMMENT "Gerando amostra H.264 720p30")
  add_custom_target(amostra720 ALL DEPENDS ${AMOSTRA720})
  add_dependencies(teste_logica_fonte amostra720)
  set_property(TEST obs_logica_fonte APPEND PROPERTY ENVIRONMENT "FMCAM_AMOSTRA720=${AMOSTRA720}")
  add_test(NAME obs_headless COMMAND teste_headless_obs "${PLUGIN_INSTALADO}"
           $<TARGET_FILE:iphone-falso> ${CMAKE_BINARY_DIR}/amostra.h264 ${AMOSTRA720})
  set_tests_properties(obs_headless PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()
