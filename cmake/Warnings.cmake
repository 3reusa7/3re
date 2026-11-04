# Compiler settings applied to Pixel3D's own targets (never to vendored code).
include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-fsanitize=undefined -fsanitize-trap=undefined" PIXEL3D_HAS_UBSAN_TRAP)
if(NOT PIXEL3D_HAS_UBSAN_TRAP)
  message(WARNING "UBSan trap mode not supported by this compiler; Debug builds run without it")
endif()

function(pixel3d_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
  if(PIXEL3D_HAS_UBSAN_TRAP)
    # Trap mode needs no runtime library, which MinGW does not ship.
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Debug>:-fsanitize=undefined -fsanitize-trap=undefined>)
  endif()
endfunction()

# Executables: fully static MinGW runtime in Release so the .exe runs anywhere.
function(pixel3d_executable target)
  pixel3d_warnings(${target})
  target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-static -static-libgcc -static-libstdc++>)
endfunction()
