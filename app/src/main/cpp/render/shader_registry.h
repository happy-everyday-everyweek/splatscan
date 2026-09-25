#pragma once

#include <cstddef>
#include <cstdint>

namespace splatscan {
namespace shaders {

/** 编译进 so 的 SPIR-V 模块，由构建期生成。 */
const uint32_t* splatCompute(size_t* sizeInBytes);
const uint32_t* blitVertex(size_t* sizeInBytes);
const uint32_t* blitFragment(size_t* sizeInBytes);

}  // namespace shaders
}  // namespace splatscan