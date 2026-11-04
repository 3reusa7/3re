# ONNX Runtime (CPU) for 3re segmentation: headers at build time, onnxruntime.dll loaded at run time
# (apps/segmentation/ort_loader.cpp), so the application starts without it.
# Pinned: official release onnxruntime-win-x64-1.22.0.zip
#   SHA-256 174c616efc0271194488642a72f1a514e01487da4dfe84c49296d66e40ebe0da
#   lib/onnxruntime.dll SHA-256 579b636403983254346a5c1d80bd28f1519cd1e284cd204f8d4ff41f8d711559 (Microsoft Authenticode)
# Store: .toolchain/onnxruntime/1.22.0 (tools/stores.ps1). Licence: MIT (LICENSES/onnxruntime-MIT.txt).
set(THREERE_ORT_VERSION 1.22.0)
set(THREERE_ORT_DIR ${CMAKE_SOURCE_DIR}/.toolchain/onnxruntime/${THREERE_ORT_VERSION})
if(NOT EXISTS ${THREERE_ORT_DIR}/include/onnxruntime_c_api.h)
  message(FATAL_ERROR "ONNX Runtime ${THREERE_ORT_VERSION} is missing: run tools/stores.ps1 (see docs/CANONICAL.md)")
endif()
set(THREERE_ORT_INCLUDE ${THREERE_ORT_DIR}/include)
set(THREERE_ORT_DLL ${THREERE_ORT_DIR}/lib/onnxruntime.dll)
set(THREERE_SAM_DIR ${CMAKE_SOURCE_DIR}/.toolchain/models/sam2.1_hiera_tiny_20260221/extracted)
