// Dumps function entries, sizes and callers, plus defined strings, for a program.
// @category RTA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = askFile("Output file", "Save");
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                w.printf("FUNC %s %s %d callers=%d%n", f.getEntryPoint(), f.getName(),
                        f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size());
            }
            DataIterator di = currentProgram.getListing().getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                if (d.hasStringValue()) {
                    w.printf("STR %s %s%n", d.getAddress(), String.valueOf(d.getValue()).replace("\n", "\\n"));
                }
            }
        }
    }
}
