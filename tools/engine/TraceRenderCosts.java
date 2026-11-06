// Read-only Ghidra audit. Output belongs in an ignored artifact directory.
// Arguments: maximum caller depth, then module-relative A32 seed addresses.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FlowOverride;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.util.ArrayDeque;
import java.util.HashSet;

public class TraceRenderCosts extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        int maxDepth = Integer.parseInt(args[0]);
        ArrayDeque<Function> queue = new ArrayDeque<>();
        ArrayDeque<Integer> depths = new ArrayDeque<>();
        HashSet<String> seen = new HashSet<>();
        DecompInterface decompiler = new DecompInterface();
        // The unrelocated ELF PLT shares the lazy resolver. Old analysis
        // incorrectly propagates that resolver's no-return property to libc
        // imports. Repair metadata in this read-only session, never input bytes.
        FunctionIterator imports = currentProgram.getFunctionManager().getFunctions(true);
        while (imports.hasNext()) {
            Function f = imports.next();
            long address = f.getEntryPoint().getOffset();
            if (address >= 0x13ef000L && address < 0x13f2000L) {
                if (f.isThunk()) f.setThunkedFunction(null);
                f.setNoReturn(false);
            }
        }
        decompiler.openProgram(currentProgram);
        println("COST_PROGRAM name=" + currentProgram.getName() +
            " sha256=" + currentProgram.getExecutableSHA256() +
            " image_base=" + currentProgram.getImageBase());
        for (int i = 1; i < args.length; i++) {
            Function f = getFunctionAt(toAddr(Long.decode(args[i])));
            if (f == null) f = getFunctionContaining(toAddr(Long.decode(args[i])));
            if (f == null) {
                println("COST_UNKNOWN no-function seed=" + args[i]);
            } else {
                queue.add(f); depths.add(0);
            }
        }
        while (!queue.isEmpty() && !monitor.isCancelled()) {
            Function f = queue.remove();
            int depth = depths.remove();
            if (!seen.add(f.getEntryPoint().toString())) continue;
            f.setNoReturn(false);
            // Discover previously pruned call fallthroughs before recalculating
            // this body's flow extent. No automatic timing claims follow.
            for (int pass = 0; pass < 4; pass++) {
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    if (ins.getFlowType().isCall()) {
                        ins.setFlowOverride(FlowOverride.NONE);
                        disassemble(ins.getAddress().add(4));
                    }
                }
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            }
            decompiler.flushCache();
            println("COST_FUNCTION depth=" + depth + " entry=" + f.getEntryPoint() +
                " name=" + f.getName() + " body=" + f.getBody());
            ReferenceIterator refs = currentProgram.getReferenceManager()
                .getReferencesTo(f.getEntryPoint());
            while (refs.hasNext()) {
                Reference ref = refs.next();
                Function caller = getFunctionContaining(ref.getFromAddress());
                println("COST_INCOMING site=" + ref.getFromAddress() +
                    " type=" + ref.getReferenceType() + " caller=" +
                    (caller == null ? "unknown" : caller.getEntryPoint()));
                if (caller != null && ref.getReferenceType().isCall() && depth < maxDepth) {
                    queue.add(caller); depths.add(depth + 1);
                }
            }
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                if (ins.getFlowType().isCall()) {
                    println("COST_CALL site=" + ins.getAddress() + " asm=" + ins);
                }
            }
            DecompileResults result = decompiler.decompileFunction(f, 90, monitor);
            if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                println("COST_PSEUDO_BEGIN entry=" + f.getEntryPoint());
                println(result.getDecompiledFunction().getC());
                println("COST_PSEUDO_END entry=" + f.getEntryPoint());
            } else {
                println("COST_UNKNOWN decompile=" + f.getEntryPoint() +
                    " error=" + result.getErrorMessage());
            }
        }
        decompiler.dispose();
    }
}
