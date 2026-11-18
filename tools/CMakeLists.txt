add_executable(iphone-falso iphone-falso/main.cc)
target_link_libraries(iphone-falso PRIVATE fmappcam_core)

add_executable(visor visor/main.cc)
target_link_libraries(visor PRIVATE fmappcam_core)

if(NOT WIN32)
  add_executable(usbmuxd-falso usbmuxd-falso/main.cc)
  target_include_directories(usbmuxd-falso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(usbmuxd-falso PRIVATE fmappcam_core)

  add_executable(teste_e2e tests/e2e.cc)
  target_link_libraries(teste_e2e PRIVATE fmappcam_core)

  # Conexao::parar() rapido em connect pendurado, usbmuxd mudo e callback lento
  add_executable(teste_conexao_parar tests/teste_conexao_parar.cc)
  target_include_directories(teste_conexao_parar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(teste_conexao_parar PRIVATE fmappcam_core)
  add_test(NAME conexao_parar COMMAND teste_conexao_parar)
  set_tests_properties(conexao_parar PROPERTIES TIMEOUT 120)
endif()

# Amostra H.264 para o iphone-falso (precisa do ffmpeg na linha de comando)
find_program(FFMPEG_BIN ffmpeg)
set(AMOSTRA ${CMAKE_BINARY_DIR}/amostra.h264)
if(FFMPEG_BIN AND NOT WIN32)
  add_custom_command(
    OUTPUT ${AMOSTRA}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/amostra/gerar.sh ${AMOSTRA}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/amostra/gerar.sh
    COMMENT "Gerando amostra H.264 1080p30")
  add_custom_target(amostra ALL DEPENDS ${AMOSTRA})

  set(E2E $<TARGET_FILE:teste_e2e>)
  set(ARGS_E2E $<TARGET_FILE:iphone-falso> $<TARGET_FILE:visor> $<TARGET_FILE:usbmuxd-falso> ${AMOSTRA}
               ${CMAKE_BINARY_DIR}/e2e)
  foreach(modo tcp usb desvio perda trava multi reconexao lento)
    add_test(NAME e2e_${modo} COMMAND ${E2E} ${modo} ${ARGS_E2E})
    set_tests_properties(e2e_${modo} PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
  endforeach()
else()
  message(WARNING "ffmpeg ou POSIX indisponivel: testes ponta a ponta nao criados")
endif()
