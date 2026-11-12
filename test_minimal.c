/* Minimal test - just create log and exit */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>

#define LOG_PATH "ux0:/data/zombiewood/debug.log"

int main(int argc, char *argv[]) {
    /* Create directory (ignore errors) */
    sceIoMkdir("ux0:/data/zombiewood", 0777);
    
    /* Open log */
    FILE *log = fopen(LOG_PATH, "w");
    if (log) {
        fprintf(log, "=== MINIMAL TEST ===\n");
        fprintf(log, "main() was called successfully!\n");
        fprintf(log, "If you see this, the eboot.bin can start.\n");
        fflush(log);
        fclose(log);
    }
    
    /* Wait a moment so file writes complete */
    sceKernelDelayThread(1000000); // 1 second
    
    return 0;
}
