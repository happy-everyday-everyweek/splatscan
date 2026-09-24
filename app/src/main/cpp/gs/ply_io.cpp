#include "gs/ply_io.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace splatscan {

namespace {

constexpr int32_t kPropertyCount = 17;

bool readLine(FILE* file, std::string& out) {
    out.clear();
    int value = 0;
    while ((value = std::fgetc(file)) != EOF) {
        if (value == '\n') return true;
        out.push_back(static_cast<char>(value));
    }
    return !out.empty();
}

}  // namespace

bool readPlyModel(const std::string& path, GaussianParams& out, Vec3& boundsCenter,
                  float& boundsRadius) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    std::string line;
    if (!readLine(file, line) || line.rfind("ply", 0) != 0) {
        std::fclose(file);
        return false;
    }

    int32_t vertexCount = 0;
    bool binaryLittleEndian = false;
    while (readLine(file, line)) {
        if (line.rfind("format", 0) == 0) {
            binaryLittleEndian = line.find("binary_little_endian") != std::string::npos;
        } else if (line.rfind("element vertex", 0) == 0) {
            vertexCount = std::atoi(line.c_str() + 14);
        } else if (line.rfind("end_header", 0) == 0) {
            break;
        }
    }

    if (!binaryLittleEndian || vertexCount <= 0) {
        std::fclose(file);
        return false;
    }

    out.ensureCapacity(vertexCount);

    float minX = 1e30f;
    float minY = 1e30f;
    float minZ = 1e30f;
    float maxX = -1e30f;
    float maxY = -1e30f;
    float maxZ = -1e30f;

    std::vector<float> buffer(kPropertyCount);
    for (int32_t i = 0; i < vertexCount; ++i) {
        if (std::fread(buffer.data(), sizeof(float), kPropertyCount, file) !=
            static_cast<size_t>(kPropertyCount)) {
            break;
        }
        // 位置 0..2、法线 3..5、球谐 DC 6..8、不透明度 9、尺度 10..12、四元数 13..16
        const float px = buffer[0];
        const float py = buffer[1];
        const float pz = buffer[2];
        const float red = buffer[6];
        const float green = buffer[7];
        const float blue = buffer[8];
        const float opacity = buffer[9];
        const float scaleX = buffer[10];
        const float scaleY = buffer[11];

        out.add({px, py, pz}, scaleX, scaleY, {red, green, blue}, opacity);

        minX = px < minX ? px : minX;
        minY = py < minY ? py : minY;
        minZ = pz < minZ ? pz : minZ;
        maxX = px > maxX ? px : maxX;
        maxY = py > maxY ? py : maxY;
        maxZ = pz > maxZ ? pz : maxZ;
    }
    std::fclose(file);

    boundsCenter = {(minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f};
    const float dx = maxX - minX;
    const float dy = maxY - minY;
    const float dz = maxZ - minZ;
    const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    boundsRadius = diagonal > 1e-3f ? diagonal * 0.5f : 1.0f;
    return out.count > 0;
}

}  // namespace splatscan