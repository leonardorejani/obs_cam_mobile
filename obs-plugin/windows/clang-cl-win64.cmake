# Toolchain de compilacao cruzada Mac -> Windows x64 (clang-cl + lld-link + SDK/CRT do xwin).
# Usada por obs-plugin/scripts/compilar-windows.sh. Nao afeta o build do Mac.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(FMCAM_LLVM_BIN "/opt/homebrew/opt/llvm/bin" CACHE PATH "bin do LLVM (clang-cl, llvm-lib, llvm-rc)")
set(FMCAM_LLD_BIN "/opt/homebrew/opt/lld/bin" CACHE PATH "bin do lld (lld-link)")
set(FMCAM_XWIN "" CACHE PATH "saida do xwin splat (crt/ e sdk/)")
if(NOT FMCAM_XWIN)
  set(FMCAM_XWIN "$ENV{FMCAM_XWIN}")
endif()

set(CMAKE_C_COMPILER   "${FMCAM_LLVM_BIN}/clang-cl")
set(CMAKE_CXX_COMPILER "${FMCAM_LLVM_BIN}/clang-cl")
set(CMAKE_LINKER       "${FMCAM_LLD_BIN}/lld-link")
set(CMAKE_AR           "${FMCAM_LLVM_BIN}/llvm-lib")
set(CMAKE_RC_COMPILER  "${FMCAM_LLVM_BIN}/llvm-rc")
set(CMAKE_MT           "")
set(CMAKE_C_COMPILER_TARGET   x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_inc
  "/imsvc ${FMCAM_XWIN}/crt/include"
  "/imsvc ${FMCAM_XWIN}/sdk/include/ucrt"
  "/imsvc ${FMCAM_XWIN}/sdk/include/um"
  "/imsvc ${FMCAM_XWIN}/sdk/include/shared")
string(JOIN " " _inc_s ${_inc})
set(CMAKE_C_FLAGS_INIT   "--target=x86_64-pc-windows-msvc ${_inc_s}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_inc_s}")

set(_libs
  "/libpath:${FMCAM_XWIN}/crt/lib/x86_64"
  "/libpath:${FMCAM_XWIN}/sdk/lib/um/x86_64"
  "/libpath:${FMCAM_XWIN}/sdk/lib/ucrt/x86_64")
string(JOIN " " _libs_s ${_libs})
foreach(t EXE SHARED MODULE)
  set(CMAKE_${t}_LINKER_FLAGS_INIT "${_libs_s}")
endforeach()

# CMake nao consegue rodar o executavel de teste do compilador: testa so a compilacao
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CROSSCOMPILING TRUE)
