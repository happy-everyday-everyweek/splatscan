package com.splatscan.app.data

import java.io.File

/**
 * 一条扫描记录。元信息用简单的键值文本保存，不依赖 JSON 库，
 * 这样存取逻辑可以在 JVM 单元测试里直接验证。
 */
data class ScanRecord(
    val id: String,
    val name: String,
    val createdAtMillis: Long,
    val durationMillis: Long,
    val gaussianCount: Int,
    val meanAccuracy: Int,
    val modelFileName: String,
) {
    fun toMetaText(): String = buildString {
        append("id=").append(id).append('\n')
        append("name=").append(name).append('\n')
        append("createdAtMillis=").append(createdAtMillis).append('\n')
        append("durationMillis=").append(durationMillis).append('\n')
        append("gaussianCount=").append(gaussianCount).append('\n')
        append("meanAccuracy=").append(meanAccuracy).append('\n')
        append("modelFileName=").append(modelFileName).append('\n')
    }

    val durationText: String
        get() {
            val seconds = durationMillis / 1000
            return "%d:%02d".format(seconds / 60, seconds % 60)
        }

    companion object {
        fun parse(text: String, fallbackId: String): ScanRecord? {
            val map = HashMap<String, String>()
            text.lineSequence().forEach { line ->
                val index = line.indexOf('=')
                if (index > 0) map[line.substring(0, index).trim()] = line.substring(index + 1).trim()
            }
            if (map.isEmpty()) return null
            return ScanRecord(
                id = map["id"] ?: fallbackId,
                name = map["name"] ?: fallbackId,
                createdAtMillis = map["createdAtMillis"]?.toLongOrNull() ?: 0L,
                durationMillis = map["durationMillis"]?.toLongOrNull() ?: 0L,
                gaussianCount = map["gaussianCount"]?.toIntOrNull() ?: 0,
                meanAccuracy = map["meanAccuracy"]?.toIntOrNull() ?: -1,
                modelFileName = map["modelFileName"] ?: "model.ply",
            )
        }
    }
}

/**
 * 扫描结果仓库。根目录由调用方给出（应用私有目录），
 * 每次扫描一个子目录，模型与元信息放在里面。
 */
class ScanStore(private val root: File) {

    fun list(): List<ScanRecord> {
        val dirs = root.listFiles { file -> file.isDirectory } ?: return emptyList()
        return dirs.mapNotNull { readMeta(it) }.sortedByDescending { it.createdAtMillis }
    }

    fun recordDir(id: String): File = File(root, id)

    fun modelFile(id: String): File {
        val record = readMeta(recordDir(id))
        return File(recordDir(id), record?.modelFileName ?: "model.ply")
    }

    fun save(record: ScanRecord): File {
        val dir = recordDir(record.id)
        dir.mkdirs()
        File(dir, META_FILE).writeText(record.toMetaText())
        return dir
    }

    fun rename(id: String, newName: String): ScanRecord? {
        val record = readMeta(recordDir(id)) ?: return null
        val updated = record.copy(name = newName)
        save(updated)
        return updated
    }

    fun delete(id: String): Boolean {
        val dir = recordDir(id)
        return dir.deleteRecursively()
    }

    fun readMeta(dir: File): ScanRecord? {
        val meta = File(dir, META_FILE)
        if (!meta.isFile) return null
        return ScanRecord.parse(meta.readText(), dir.name)
    }

    fun totalBytes(): Long {
        val dirs = root.listFiles() ?: return 0L
        return dirs.sumOf { dir -> dir.walkBottomUp().filter { it.isFile }.sumOf { it.length() } }
    }

    private companion object {
        const val META_FILE = "meta.txt"
    }
}