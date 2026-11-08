// Lists indirect calls through [reg+off] for given vtable offsets, with containing function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class FindVCalls extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    InstructionIterator it = currentProgram.getListing().getInstructions(true);
    while (it.hasNext()) {
      Instruction ins = it.next();
      if (!ins.getMnemonicString().equals("CALL")) continue;
      String s = ins.toString();
      for (int i = 1; i < a.length; i++) {
        if (s.contains(" + " + a[i] + "]")) {
          Function f = getFunctionContaining(ins.getAddress());
          out.println(a[i] + " " + ins.getAddress() + " " + s + "  in " + (f == null ? "?" : f.getName()));
        }
      }
    }
    out.close();
  }
}
