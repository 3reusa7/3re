// tinygltf is used only to RE-LOAD our own GLB output as an independent check.
// stb_image's implementation lives in image/image.cpp; only its declarations are
// needed here (PNG decoding of the embedded atlas).
#include <nlohmann/json.hpp>

#define STBI_NO_STDIO
#include <stb_image.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <tiny_gltf.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
