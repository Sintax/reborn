// Writes every function as {"name","start","end"} with addresses relative to the image base.
// @category Export
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "functions.json";
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.print("[");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                long s = f.getEntryPoint().getOffset() - base;
                long e = f.getBody().getMaxAddress().getOffset() - base;
                String name = f.getName().replace("\\", "\\\\").replace("\"", "\\\"");
                if (!first) w.print(",");
                first = false;
                w.print("{\"name\":\"" + name + "\",\"start\":" + s + ",\"end\":" + e + "}");
            }
            w.print("]");
        }
    }
}
