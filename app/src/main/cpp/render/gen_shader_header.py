"""把着色器编译产物 .spv 转成 C++ 头文件里的常量数组。

glslangValidator 输出的是二进制 SPIR-V，直接嵌进 so 比运行时读资产简单，
也避免额外的资源加载代码。构建时由 CMake 调用。
"""

import os
import sys

HEADER = """// 由 tools 脚本自动生成，请勿手改。源文件在 render/shaders/。
#pragma once

#include <cstddef>
#include <cstdint>

namespace splatscan {
namespace shaders {

"""

FOOTER = """}  // namespace shaders
}  // namespace splatscan
"""


def emit(f, name, data):
    f.write("inline const uint32_t k%s[] = {\n" % name)
    for i in range(0, len(data), 4):
        chunk = data[i:i + 4]
        value = int.from_bytes(chunk, "little")
        f.write("    0x%08Xu, " % value)
        if (i // 4) % 6 == 5:
            f.write("\n")
    f.write("\n};\n\n")
    f.write("inline const size_t k%sSize = sizeof(k%s);\n\n" % (name, name))


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法: gen_shader_header.py <spv目录> <输出头文件>")
    spv_dir = sys.argv[1]
    out_path = sys.argv[2]

    names = {
        "splat.comp.spv": "SplatComputeSpv",
        "blit.vert.spv": "BlitVertexSpv",
        "blit.frag.spv": "BlitFragmentSpv",
    }

    with open(out_path, "w") as f:
        f.write(HEADER)
        for file_name, symbol in names.items():
            path = os.path.join(spv_dir, file_name)
            with open(path, "rb") as spv_file:
                emit(f, symbol, spv_file.read())
        f.write(FOOTER)


if __name__ == "__main__":
    main()
