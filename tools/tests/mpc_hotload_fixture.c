#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Run only in the disposable --self-test child. Mark constructor/destructor. */
static void mark(const char *phase) {
    char path[128];
    const char *dir=getenv("MPC_NATIVE_FIXTURE_DIR");if(!dir)dir="/tmp";
    snprintf(path,sizeof path,"%s/mpc-native-fixture-%d-%s",dir,getpid(),phase);
    int fd=open(path,O_CREAT|O_WRONLY|O_EXCL,0600);
    if(fd>=0){ssize_t written=write(fd,"ok\n",3);(void)written;close(fd);}
}
__attribute__((constructor)) static void begin(void){mark("loaded");}
__attribute__((destructor)) static void end(void){mark("unloaded");}
