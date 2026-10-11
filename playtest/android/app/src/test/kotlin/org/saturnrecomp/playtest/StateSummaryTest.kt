package org.saturnrecomp.playtest

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.DataOutputStream
import java.io.File

class StateSummaryTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test
    fun readsKeptBytesFromBothRamRegionsInSaturnOrder() {
        val file = tmp.newFile()
        writeStateSummaryFixture(file, mapOf(0x00200010L to byteArrayOf(0x12, 0x34), 0x06000020L to byteArrayOf(0x80.toByte(), 0xFF.toByte())))
        assertEquals("9000 1234 80FF", StateSummary.line(file, "00200010:2,06000020:2", 9000))
        assertEquals("9000 80FF", StateSummary.line(file, "26000020:2", 9000))
    }

    @Test
    fun rejectsMissingUnsupportedAndTruncatedDumps() {
        val file = tmp.newFile()
        assertNull(StateSummary.line(file, "06000000:1", 0))
        assertNull(StateSummary.line(File(tmp.root, "missing"), "06000000:1", 0))
        writeStateSummaryFixture(file, emptyMap(), version = 2)
        assertNull(StateSummary.line(file, "06000000:1", 0))
        writeStateSummaryFixture(file, emptyMap())
        val valid = file.readBytes()
        for (length in listOf(7, 11, 20, valid.size - 1)) {
            file.writeBytes(valid.copyOf(length))
            assertNull(StateSummary.line(file, "06000000:1", 0))
        }
        file.writeBytes(valid.also { it[0] = 0 })
        assertNull(StateSummary.line(file, "06000000:1", 0))
    }

    @Test
    fun rejectsMissingOrShortCoreParts() {
        val file = tmp.newFile()
        writeStateSummaryFixture(file, emptyMap(), partName = "memory")
        assertNull(StateSummary.line(file, "06000000:1", 0))
        writeStateSummaryFixture(file, emptyMap(), coreSize = 0x100001)
        assertNull(StateSummary.line(file, "06000000:1", 0))
    }

    @Test
    fun rejectsRangesOutsideRamAndAcrossItsBoundary() {
        val file = tmp.newFile()
        writeStateSummaryFixture(file, emptyMap())
        for (keep in listOf("", "bad", "06000000:no", "06000000:0", "06000000:-1", "060FFFFF:2", "002FFFFF:2", "05FFFFFF:1", "06100000:1")) {
            assertNull(keep, StateSummary.line(file, keep, 0))
        }
        assertEquals("0 00", StateSummary.line(file, "060FFFFF:1", 0))
    }
}

internal fun writeStateSummaryFixture(file: File, values: Map<Long, ByteArray>, version: Int = 1, partName: String = "core", coreSize: Int = 0x200000) {
    val core = ByteArray(coreSize)
    for ((address, bytes) in values) {
        val offset = if (address < 0x06000000) address - 0x00200000 else address - 0x06000000 + 0x100000
        bytes.copyInto(core, offset.toInt())
    }
    DataOutputStream(file.outputStream()).use { out ->
        out.writeBytes("SATSTATE")
        out.writeInt(Integer.reverseBytes(version))
        for ((name, bytes) in listOf("machine" to byteArrayOf(1, 2, 3), partName to core, "video" to byteArrayOf(4))) {
            out.writeByte(name.length)
            out.writeBytes(name)
            out.writeLong(java.lang.Long.reverseBytes(bytes.size.toLong()))
            out.write(bytes)
        }
    }
}
