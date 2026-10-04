// Export the functions Ghidra's auto-analysis found, as "ENTRY<TAB>SIZE" lines,
// to compare with saturnrecomp.recomp.discover on the same program.
//
//   analyzeHeadless PROJDIR NAME -import FILE.BIN -overwrite \
//       -processor SuperH:BE:32:SH-2 -loader BinaryLoader -loader-baseAddr 0x0600B000 \
//       -scriptPath saturn-recomp/tools/ghidra -postScript ExportFuncs.java OUT.tsv
//
// (JAVA_HOME must point at a JDK 21 for Ghidra 12.)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportFuncs extends GhidraScript {
    public void run() throws Exception {
        PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0]));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            w.println(String.format("%08X\t%d", f.getEntryPoint().getOffset(),
                                    f.getBody().getNumAddresses()));
        }
        w.close();
    }
}
