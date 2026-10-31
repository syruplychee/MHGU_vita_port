# A KeepRunning title and its observer must survive the launching exec job.
# Only break away when the parent job explicitly permits it; never change job
# policy globally. Each caller receives and owns a retained Process handle.
if (-not ('VPDetachedProcess' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class VPDetachedProcess {
    public static uint LastCreationFlags;
    public static bool LastChildInJob;
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Startup {
        public uint cb; public string reserved, desktop, title;
        public uint x,y,xSize,ySize,xChars,yChars,fill,flags;
        public ushort show,reservedBytes; public IntPtr reservedData,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Created { public IntPtr process,thread; public uint processId,threadId; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool IsProcessInJob(IntPtr process,IntPtr job,out bool result);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int kind,IntPtr info,int length,out int returned);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CreateProcessW(string app,StringBuilder command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup startup,out Created created);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    static string Quote(string arg) {
        if (arg == null) throw new ArgumentNullException("arg");
        var b=new StringBuilder("\""); int slashes=0;
        foreach(char c in arg) {
            if(c=='\\') { slashes++; continue; }
            b.Append('\\', c=='"' ? slashes*2+1 : slashes);
            slashes=0; b.Append(c);
        }
        b.Append('\\',slashes*2); b.Append('"'); return b.ToString();
    }
    public static Process Start(string executable,string[] args,string cwd,bool hidden) {
        bool inJob;
        if(!IsProcessInJob(GetCurrentProcess(),IntPtr.Zero,out inJob))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        uint flags=hidden ? 0x08000000u : 0u;
        if(inJob) {
            int size=IntPtr.Size==8 ? 64 : 48; // JOBOBJECT_BASIC_LIMIT_INFORMATION
            IntPtr info=Marshal.AllocHGlobal(size);
            try {
                int returned;
                if(!QueryInformationJobObject(IntPtr.Zero,2,info,size,out returned))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                uint limits=unchecked((uint)Marshal.ReadInt32(info,16));
                if((limits & 0x0800u)!=0) flags|=0x01000000u; // BREAKAWAY_OK
                else if((limits & 0x1000u)==0)
                    throw new InvalidOperationException("Parent job prohibits persistent child breakaway");
            } finally { Marshal.FreeHGlobal(info); }
        }
        var command=new StringBuilder(Quote(executable));
        foreach(string arg in args) command.Append(' ').Append(Quote(arg));
        var startup=new Startup(); startup.cb=(uint)Marshal.SizeOf(typeof(Startup));
        if(hidden) {startup.flags=1; startup.show=0;}
        Created created;
        if(!CreateProcessW(executable,command,IntPtr.Zero,IntPtr.Zero,false,flags,
                          IntPtr.Zero,cwd,ref startup,out created))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        Process process=null;
        try {
            process=Process.GetProcessById((int)created.processId);
            IntPtr retained=process.Handle;
            bool childInJob;
            if(!IsProcessInJob(created.process,IntPtr.Zero,out childInJob))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            // An enclosing app job can remain after breaking away from the
            // launcher's innermost job. Record this instead of claiming that
            // the process escaped every possible enclosing job.
            LastCreationFlags=flags; LastChildInJob=childInJob;
            return process;
        } catch {
            if(process!=null) {try {if(!process.HasExited) {process.Kill();process.WaitForExit(5000);}}catch {}}
            throw;
        } finally {CloseHandle(created.thread);CloseHandle(created.process);}
    }
}
'@
}

function Start-VPDetachedProcess {
    param([string]$FilePath, [string[]]$Arguments=@(), [string]$WorkingDirectory=$PWD.Path, [switch]$Hidden)
    [VPDetachedProcess]::Start($FilePath,$Arguments,$WorkingDirectory,[bool]$Hidden)
}
