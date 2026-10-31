#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

/* A native build entry, not a loader or a claim of game execution. */
int main(void) {
    const char *directory = "ux0:data/mhgu-vita-port";
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) {
        return 1;
    }
    FILE *log = fopen("ux0:data/mhgu-vita-port/bootstrap.log", "wb");
    if (log == NULL) {
        return 2;
    }
    int written = fputs("MHGU Vita Port bootstrap\n"
                        "Target: update-1.4.0 / native ARMv7\n"
                        "Status: project initialized; game loader not implemented\n", log);
    int closed = fclose(log);
    return written < 0 || closed != 0 ? 3 : 0;
}
