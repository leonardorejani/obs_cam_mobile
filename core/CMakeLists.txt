find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

add_library(fmappcam_core STATIC
  rede.cc
  protocolo.cc
  usbmux.cc
  conexao.cc
  decodificador.cc)
target_include_directories(fmappcam_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fmappcam_core PUBLIC PkgConfig::FFMPEG Threads::Threads)
if(WIN32)
  target_link_libraries(fmappcam_core PUBLIC ws2_32)
endif()

# Testes unitarios
add_library(fmappcam_teste STATIC tests/teste_main.cc)
target_include_directories(fmappcam_teste PUBLIC tests)
target_link_libraries(fmappcam_teste PUBLIC fmappcam_core)

foreach(nome teste_protocolo teste_usbmux)
  add_executable(${nome} tests/${nome}.cc)
  target_include_directories(${nome} PRIVATE ${CMAKE_SOURCE_DIR}/tools)
  target_link_libraries(${nome} PRIVATE fmappcam_teste)
  add_test(NAME ${nome} COMMAND ${nome})
endforeach()
