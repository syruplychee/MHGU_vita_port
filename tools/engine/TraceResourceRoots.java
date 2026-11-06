// Read-only original-game owner discovery. Never save the analysis database.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.FlowOverride;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.LinkedHashMap;

public class TraceResourceRoots extends GhidraScript {
    @Override public void run() throws Exception {
        println("OWNER_PROGRAM sha256=" + currentProgram.getExecutableSHA256());
        FunctionIterator imports = currentProgram.getFunctionManager().getFunctions(true);
        while (imports.hasNext()) {
            Function f = imports.next();
            long at = f.getEntryPoint().getOffset();
            if (at >= 0x13ef000L && at < 0x13f2000L) {
                if (f.isThunk()) f.setThunkedFunction(null);
                f.setNoReturn(false);
            }
        }
        ArrayDeque<Address> queue = new ArrayDeque<>();
        ArrayDeque<Integer> depth = new ArrayDeque<>();
        HashSet<String> seen = new HashSet<>();
        LinkedHashMap<String, Function> functions = new LinkedHashMap<>();
        for (String arg : getScriptArgs()) {
            Address address = toAddr(Long.decode(arg));
            queue.add(address); depth.add(0);
            Function f = getFunctionAt(address);
            if (f == null && currentProgram.getMemory().getBlock(address).isExecute()) {
                disassemble(address);
                f = createFunction(address, null);
            }
            if (f != null) functions.put(f.getEntryPoint().toString(), f);
        }
        while (!queue.isEmpty() && seen.size() < 150 && !monitor.isCancelled()) {
            Address target = queue.remove(); int level = depth.remove();
            if (!seen.add(target.toString())) continue;
            println("OWNER_NODE address=" + target + " depth=" + level);
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
            int count = 0;
            while (refs.hasNext() && count++ < 60) {
                Reference ref = refs.next();
                Address from = ref.getFromAddress();
                Function f = getFunctionContaining(from);
                println("OWNER_REF from=" + from + " to=" + target + " type=" +
                    ref.getReferenceType() + " function=" + (f == null ? "none" : f.getEntryPoint()));
                if (f != null) functions.put(f.getEntryPoint().toString(), f);
                else if (level < 3) { queue.add(from); depth.add(level + 1); }
            }
        }
        println("OWNER_FUNCTION_COUNT count=" + functions.size());
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        int count = 0;
        for (Function f : functions.values()) {
            println("OWNER_FUNCTION entry=" + f.getEntryPoint() + " name=" + f.getName());
            if (count++ >= 4) continue;
            f.setNoReturn(false);
            for (int pass = 0; pass < 4; ++pass) {
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    if (ins.getFlowType().isCall()) {
                        ins.setFlowOverride(FlowOverride.NONE);
                        disassemble(ins.getAddress().add(4));
                    }
                }
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            }
            decompiler.flushCache();
            var result = decompiler.decompileFunction(f, 8, monitor);
            if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                println("OWNER_PSEUDO_BEGIN entry=" + f.getEntryPoint());
                println(result.getDecompiledFunction().getC());
                println("OWNER_PSEUDO_END entry=" + f.getEntryPoint());
            } else println("OWNER_UNKNOWN entry=" + f.getEntryPoint() + " error=" + result.getErrorMessage());
        }
        decompiler.dispose();
        println("OWNER_LIMITS nodes=150 refs_per_node=60 depth=3 decompiled_functions=4");
    }
}
