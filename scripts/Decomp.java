// Ghidra headless postScript: decompile functions at addresses given as args
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      Address ad = toAddr(a[i]);
      Function f = getFunctionContaining(ad);
      if (f == null) { createFunction(ad, null); f = getFunctionAt(ad); }
      if (f == null) { out.println("// no function at " + a[i]); continue; }
      out.println("// ===== " + a[i] + " -> " + f.getName() + " @ " + f.getEntryPoint());
      DecompileResults r = d.decompileFunction(f, 120, monitor);
      out.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "// decompile failed");
    }
    out.close();
  }
}
