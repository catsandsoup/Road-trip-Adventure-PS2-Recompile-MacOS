// Dumps disassembly for "start-end" hex ranges given as script arguments (after the output path).
// @category RTA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DumpRanges extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                String[] p = args[i].split("-");
                Address start = toAddr(Long.parseLong(p[0], 16));
                Address end = toAddr(Long.parseLong(p[1], 16));
                w.printf("== %s ==%n", args[i]);
                InstructionIterator it = currentProgram.getListing().getInstructions(new AddressSet(start, end), true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    w.printf("%s  %s%n", ins.getAddress(), ins.toString());
                }
            }
        }
    }
}
