package org.saturnrecomp.playtest

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

object StateSummary {
    // state.cpp's version 1 core part starts with low WRAM, then high WRAM, one MiB each.
    fun line(file: File, keep: String, vblank: Long): String? = try {
        val ranges = keep.split(",").map { range ->
            val fields = range.split(":")
            require(fields.size == 2)
            val address = fields[0].toLong(16) and 0x1FFFFFFF
            val size = fields[1].toInt()
            val offset = when (address) {
                in 0x00200000L..0x002FFFFFL -> address - 0x00200000
                in 0x06000000L..0x060FFFFFL -> address - 0x06000000 + 0x100000
                else -> return null
            }
            require(size > 0 && size <= 0x100000 - (offset.toInt() and 0xFFFFF))
            offset to size
        }
        RandomAccessFile(file, "r").use { input ->
            val magic = ByteArray(8).also { input.readFully(it) }
            if (!magic.contentEquals("SATSTATE".toByteArray()) || Integer.reverseBytes(input.readInt()) != 1) return null
            var core: Long? = null
            while (input.filePointer < input.length()) {
                val name = ByteArray(input.readUnsignedByte()).also { input.readFully(it) }.decodeToString()
                val size = java.lang.Long.reverseBytes(input.readLong())
                if (size < 0 || size > input.length() - input.filePointer) return null
                if (name == "core") {
                    if (size < 0x200000) return null
                    core = input.filePointer
                }
                input.seek(input.filePointer + size)
            }
            val start = core ?: return null
            val values = ranges.map { (offset, size) ->
                input.seek(start + offset)
                ByteArray(size).also { input.readFully(it) }.joinToString("") { "%02X".format(it.toInt() and 255) }
            }
            "$vblank " + values.joinToString(" ")
        }
    } catch (_: IOException) {
        null
    } catch (_: IllegalArgumentException) {
        null
    }
}
