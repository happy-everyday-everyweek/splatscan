#include "render/shader_registry.h"

#include "shaders_spv.h"

namespace splatscan {
namespace shaders {

const uint32_t* splatCompute(size_t* sizeInBytes) {
    if (sizeInBytes != nullptr) *sizeInBytes = kSplatComputeSpvSize;
    return kSplatComputeSpv;
}

const uint32_t* blitVertex(size_t* sizeInBytes) {
    if (sizeInBytes != nullptr) *sizeInBytes = kBlitVertexSpvSize;
    return kBlitVertexSpv;
}

const uint32_t* blitFragment(size_t* sizeInBytes) {
    if (sizeInBytes != nullptr) *sizeInBytes = kBlitFragmentSpvSize;
    return kBlitFragmentSpv;
}

}  // namespace shaders
}  // namespace splatscan