// Writes every function of the current program as CSV: addr,size,name,is_thunk,xrefs
// Usage (headless post-script): -postScript ExportFunctionsCsv.java /path/out.csv
// The lifter reads the addr and name columns (as GunBound's PROGRESS.csv).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportFunctionsCsv extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : currentProgram.getName() + ".functions.csv");
        int n = 0;
        try (PrintWriter pw = new PrintWriter(new FileWriter(out))) {
            pw.println("addr,size,name,is_thunk,xrefs");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal()) continue;
                int xrefs = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) xrefs++;
                pw.printf("0x%08x,%d,%s,%d,%d%n",
                    f.getEntryPoint().getOffset(), f.getBody().getNumAddresses(),
                    f.getName().replace(',', '_'), f.isThunk() ? 1 : 0, xrefs);
                n++;
            }
        }
        println("ExportFunctionsCsv: " + n + " functions -> " + out);
    }
}
