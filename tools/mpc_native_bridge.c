#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "native_profile.h"
#include "mpc_native.h"

/* Bounded lab bridge. No arbitrary calls, raw writes, saves, or transport commands.
 * Commands are local/root-only. PULSE restores its original requested value. */
_Static_assert(sizeof(void*)==4,"32-bit ARM only");
struct flags { uint32_t track,program; unsigned mute,solo,arm,audio_solo,audio_arm,silenced; };
struct job {
    pthread_mutex_t mutex; pthread_cond_t cond; int done,cancelled,refs,op,property,value,expected,ok;
    uint32_t project,track,program; pid_t tid; unsigned before,count; char error[128]; struct flags tracks[128];
};
struct function { void *data[2]; int (*manager)(void*,const void*,int); void (*invoke)(const void*); };
_Static_assert(sizeof(struct function)==16,"GNU ARM std::function ABI");
static uintptr_t image;
static struct mpc_native_adapter *adapter;
static int memory=-1;
static pid_t ui_tid;
static int pending_captures;
static char socket_path[108];
static int read_mem(uintptr_t p,void *out,size_t n) { return pread(memory,out,n,(off_t)p)==(ssize_t)n ? 0 : -1; }
static int get32(uintptr_t p,uint32_t *v) { return read_mem(p,v,4); }
static void release(struct job *j) {
    if(__atomic_sub_fetch(&j->refs,1,__ATOMIC_ACQ_REL)==0){pthread_cond_destroy(&j->cond);pthread_mutex_destroy(&j->mutex);free(j);}
}
static int manager(void *out,const void *in,int operation) {
    const struct function *src=in;struct function *dst=out;struct job *j=src->data[0];
    if(operation==0)dst->data[0]=NULL;
    else if(operation==1)dst->data[0]=(void*)in;
    else if(operation==2){dst->data[0]=j;dst->data[1]=NULL;__atomic_add_fetch(&j->refs,1,__ATOMIC_RELAXED);__atomic_add_fetch(&pending_captures,1,__ATOMIC_RELAXED);}
    else if(operation==3){release(j);__atomic_sub_fetch(&pending_captures,1,__ATOMIC_RELEASE);}
    return 0;
}
static int snapshot(struct job *j) {
    uint32_t d[0xaa8/4];
    if(read_mem(j->project,d,sizeof d) || d[0]!=image+PROJECT_VTABLE)return -1;
    for(size_t k=0;k<sizeof secondary/sizeof secondary[0];k++)
        if(d[secondary[k].offset/4]!=image+secondary[k].vtable)return -1;
    uint32_t pool=d[0xa94/4],p[0x50/4];
    if(read_mem(pool,p,sizeof p) || p[0]!=image+POOL_VTABLE)return -1;
    uint32_t begin=p[0x44/4],end=p[0x48/4],cap=p[0x4c/4];
    if(end<begin || cap<end || (end-begin)%4 || (cap-begin)%4 || cap-begin>128*4)return -1;
    j->count=(end-begin)/4;uint32_t tracks[128];
    if(!j->count || read_mem(begin,tracks,j->count*4))return -1;
    unsigned found=0;
    for(unsigned k=0;k<j->count;k++) {
        struct flags *s=&j->tracks[k];s->track=tracks[k];uint32_t vt,pvt;
        if(get32(s->track,&vt) || vt!=image+TRACK_VTABLE || get32(s->track+0x648,&s->program)
            || get32(s->program,&pvt))return -1;
        if(pvt!=image+PLUGIN_VTABLE && pvt!=image+DRUM_VTABLE)return -1;
        struct mpc_native_target target={j->project,s->track,s->program};
        struct mpc_native_track_state state;
        if(mpc_native_read_track(adapter,&target,&state)!=MPC_NATIVE_OK)return -1;
        s->mute=state.muted;s->solo=state.soloed;s->arm=state.record_armed;
        s->audio_solo=state.audio_soloed;s->audio_arm=state.audio_record_armed;s->silenced=state.silenced;
        if(s->track==j->track && s->program==j->program)found++;
    }
    return found==1?0:-1;
}
static void invoke(const void *capture) {
    struct job *j=((const struct function*)capture)->data[0];
    pthread_mutex_lock(&j->mutex);
    j->tid=(pid_t)syscall(SYS_gettid);
    if(j->cancelled)snprintf(j->error,sizeof j->error,"cancelled before dispatch");
    else if(j->op==0) {ui_tid=j->tid;j->ok=1;}
    else if(j->tid!=ui_tid || ui_tid!=getpid())snprintf(j->error,sizeof j->error,"unverified UI thread");
    else if(snapshot(j))snprintf(j->error,sizeof j->error,"project/track ownership or type changed");
    else {
        j->ok=1;
        if(j->op==2) {
            struct mpc_native_target target={j->project,j->track,j->program};
            struct mpc_native_track_state before;
            enum mpc_native_error error=mpc_native_read_track(adapter,&target,&before);
            bool expected=j->expected!=0,value=j->value!=0;
            struct mpc_native_write_result result;
            if(error==MPC_NATIVE_OK) {
                j->before=j->property==0?before.muted:j->property==1?before.soloed:before.record_armed;
                if(j->property==0)error=mpc_native_set_mute(adapter,&target,value,&expected,&result);
                else if(j->property==1)error=mpc_native_set_solo(adapter,&target,value,&expected,&result);
                else error=mpc_native_set_record_armed(adapter,&target,value,&expected,&result);
            }
            if(error!=MPC_NATIVE_OK){j->ok=0;snprintf(j->error,sizeof j->error,"%s",mpc_native_error_name(error));}

        }
    }
    j->done=1;pthread_cond_broadcast(&j->cond);pthread_mutex_unlock(&j->mutex);
}
static struct job *dispatch(int op,int property,int value,int expected,uint32_t project,uint32_t track,uint32_t program) {
    struct job *j=calloc(1,sizeof *j);if(!j)return NULL;
    pthread_mutex_init(&j->mutex,NULL);pthread_cond_init(&j->cond,NULL);j->refs=2;
    __atomic_add_fetch(&pending_captures,1,__ATOMIC_RELAXED);
    j->op=op;j->property=property;j->value=value;j->expected=expected;j->project=project;j->track=track;j->program=program;
    struct function f={{j,NULL},manager,invoke};
    bool queued=((bool(*)(struct function*))(image+CALL_ASYNC))(&f);
    if(f.manager)f.manager(&f,&f,3);
    pthread_mutex_lock(&j->mutex);
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=3;
    while(queued&&!j->done)if(pthread_cond_timedwait(&j->cond,&j->mutex,&until)==ETIMEDOUT)break;
    if(!j->done){j->cancelled=1;j->ok=0;snprintf(j->error,sizeof j->error,queued?"dispatch timeout; cancelled if not started":"queue rejected");}
    pthread_mutex_unlock(&j->mutex);return j;
}
static void report(FILE *f,const char *phase,struct job *j) {
    if(!j){fprintf(f,"{\"phase\":\"%s\",\"ok\":false,\"error\":\"allocation failed\"}\n",phase);fflush(f);return;}
    pthread_mutex_lock(&j->mutex);
    fprintf(f,"{\"phase\":\"%s\",\"ok\":%s,\"tid\":%d,\"pid\":%d,\"error\":\"%s\",\"tracks\":[",phase,j->ok?"true":"false",j->tid,getpid(),j->error);
    for(unsigned k=0;k<j->count;k++) {
        struct flags *s=&j->tracks[k];
        fprintf(f,"%s{\"track\":\"%08x\",\"program\":\"%08x\",\"mute\":%u,\"solo\":%u,\"arm\":%u,\"audioSolo\":%u,\"audioArm\":%u,\"silenced\":%u}",k?",":"",s->track,s->program,s->mute,s->solo,s->arm,s->audio_solo,s->audio_arm,s->silenced);
    }
    fprintf(f,"]}\n");fflush(f);pthread_mutex_unlock(&j->mutex);
}
static void pause_ms(unsigned ms) {struct timespec d={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&d,&d)&&errno==EINTR){}}
static void *worker(void *unused) {
    (void)unused;pthread_setname_np(pthread_self(),"mpc native lab");
    int sock=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(sock<0)return NULL;
    struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,socket_path);
    if(bind(sock,(struct sockaddr*)&addr,sizeof addr)||chmod(socket_path,0600)||listen(sock,2)){close(sock);return NULL;}
    int quit=0;
    while(!quit) {
        int fd=accept4(sock,NULL,NULL,SOCK_CLOEXEC);if(fd<0){if(errno==EINTR)continue;break;}
        struct ucred cred;socklen_t len=sizeof cred;
        if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len)||cred.uid!=0){close(fd);continue;}
        struct timeval timeout={15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
        FILE *f=fdopen(fd,"r+");if(!f){close(fd);continue;}
        char line[256],verb[16]={0},prop[16]={0},extra;
        if(fgets(line,sizeof line,f)) {
            if(!strcmp(line,"ADAPTER_STATUS\n")) {
                struct mpc_native_status status;
                enum mpc_native_error error=mpc_native_get_status(adapter,&status);
                if(error==MPC_NATIVE_OK)fprintf(f,"MPC_ADAPTER_V1 %s %s %s VERIFIED\n",status.instance_id,status.profile_id,status.build_id);
                else fprintf(f,"MPC_ADAPTER_ERROR %s\n",mpc_native_error_name(error));
            }
            else if(!strncmp(line,"READ ",5)) {
                uint32_t project,track,program;
                if(sscanf(line,"READ %x %x %x %c",&project,&track,&program,&extra)==3) {
                    struct job *j=dispatch(1,0,0,0,project,track,program);report(f,"read",j);if(j)release(j);
                } else fprintf(f,"{\"ok\":false,\"error\":\"invalid READ target\"}\n");
            }
            else if(!strcmp(line,"PING\n")) {struct job *j=dispatch(0,0,0,0,0,0,0);report(f,"ping",j);if(j)release(j);}
            else if(!strcmp(line,"QUIT\n")){
                if(__atomic_load_n(&pending_captures,__ATOMIC_ACQUIRE)==0){quit=1;fprintf(f,"{\"ok\":true,\"phase\":\"stopping\"}\n");}
                else fprintf(f,"{\"ok\":false,\"error\":\"callbacks still pending; retry QUIT\"}\n");
            }
            else {
                unsigned value,expected,hold;uint32_t project,track,program;
                int n=sscanf(line,"%15s %15s %u %u %u %x %x %x %c",verb,prop,&value,&expected,&hold,&project,&track,&program,&extra);
                int property=!strcmp(prop,"mute")?0:!strcmp(prop,"solo")?1:!strcmp(prop,"arm")?2:-1;
                if(n==8&&!strcmp(verb,"PULSE")&&property>=0&&value<=1&&expected<=1&&hold>=100&&hold<=10000) {
                    struct job *j=dispatch(2,property,value,expected,project,track,program);
                    int applied=j&&j->ok;unsigned original=j?j->before:0;
                    report(f,"accepted",j);if(j)release(j);
                    if(applied) {
                        pause_ms(150);j=dispatch(1,property,0,0,project,track,program);report(f,"applied",j);if(j)release(j);
                        pause_ms(hold);
                        j=dispatch(2,property,original,value,project,track,program);report(f,"restoreAccepted",j);if(j)release(j);
                        pause_ms(150);j=dispatch(1,property,0,0,project,track,program);report(f,"restored",j);if(j)release(j);
                    }
                } else {fprintf(f,"{\"ok\":false,\"error\":\"expected PING, QUIT, or bounded PULSE\"}\n");}
            }
        }
        fclose(f);
    }
    close(sock);unlink(socket_path);return NULL;
}
static int find_main(struct dl_phdr_info *p,size_t n,void *unused) {
    (void)n;(void)unused;if(p->dlpi_name&&*p->dlpi_name)return 0;
    image=p->dlpi_addr;return 1;
}
/* This hook is confined to the retained experimental lab. Adapter ownership
 * checks follow it, but authoritative active-project generations remain #20. */
static enum mpc_native_error lab_target(void *unused,const struct mpc_native_target *target) {
    (void)unused;return target?MPC_NATIVE_OK:MPC_NATIVE_INVALID_ARGUMENT;
}
__attribute__((constructor)) static void start(void) {
    dl_iterate_phdr(find_main,NULL);if(!image)return;
    struct mpc_native_hooks hooks={MPC_NATIVE_ABI,sizeof hooks,NULL,lab_target};
    if(mpc_native_open(&hooks,&adapter)!=MPC_NATIVE_OK)return;
    memory=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
    if(memory<0){mpc_native_close(adapter);adapter=NULL;return;}
    snprintf(socket_path,sizeof socket_path,"/tmp/mpc-native-lab-%d.sock",getpid());
    pthread_t t;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256*1024);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    sigset_t all,old;sigfillset(&all);pthread_sigmask(SIG_SETMASK,&all,&old);
    int rc=pthread_create(&t,&attr,worker,NULL);pthread_sigmask(SIG_SETMASK,&old,NULL);pthread_attr_destroy(&attr);
    if(rc){close(memory);memory=-1;}
}
__attribute__((destructor)) static void finish(void) {
    /* The host must QUIT and observe worker exit before dlclose. */
    if(memory>=0)close(memory);
    mpc_native_close(adapter);adapter=NULL;
}
