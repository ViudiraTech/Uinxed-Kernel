#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);failures++; } } while(0)
static int writefile(const char *p,const char *value) { int f=open(p,O_WRONLY);if(f<0)return -1;int r=write(f,value,strlen(value));close(f);return r==(int)strlen(value)?0:-1; }
static int waitgood(pid_t pid) {int s;if(waitpid(pid,&s,0)!=pid)return 0;if(!WIFEXITED(s)||WEXITSTATUS(s))printf("CHILD pid=%d status=0x%x\n",pid,s);return WIFEXITED(s)&&WEXITSTATUS(s)==0;}
int main(void) {
 setbuf(stdout,0);
 char host[64];gethostname(host,sizeof(host));
 int ns=open("/proc/self/ns/uts",O_RDONLY);struct statfs fs;
 CHECK(ns>=0);CHECK(fstatfs(ns,&fs)==0&&fs.f_type==0x6e736673);if(ns>=0)close(ns);
 int sem=semget(0x5301,1,IPC_CREAT|0600),shm=shmget(0x5302,4096,IPC_CREAT|0600),mq=msgget(0x5303,IPC_CREAT|0600);
 CHECK(sem>=0&&shm>=0&&mq>=0);
 pid_t c=fork();CHECK(c>=0);
 if(!c) {
  CHECK(unshare(CLONE_NEWIPC|CLONE_NEWUTS)==0);
  CHECK(semget(0x5301,0,0)<0&&errno==ENOENT);
  CHECK(shmget(0x5302,1,0)<0&&errno==ENOENT);
  CHECK(msgget(0x5303,0)<0&&errno==ENOENT);
  CHECK(semctl(sem,0,GETVAL)<0);CHECK(shmat(shm,0,0)==(void*)-1);
  struct msqid_ds m;CHECK(msgctl(mq,IPC_STAT,&m)<0);
  int a=semget(0x5301,1,IPC_CREAT|0600),b=shmget(0x5302,4096,IPC_CREAT|0600),d=msgget(0x5303,IPC_CREAT|0600);
  CHECK(a>=0&&b>=0&&d>=0);CHECK(sethostname("child-only",10)==0);
  _exit(failures?1:0);
 }
 CHECK(waitgood(c));char now[64];gethostname(now,sizeof(now));CHECK(!strcmp(now,host));CHECK(semctl(sem,0,GETVAL)>=0);
 c=fork();CHECK(c>=0);
 if(!c) {
  CHECK(setgid(1000)==0);CHECK(setuid(1000)==0);
  CHECK(unshare(CLONE_NEWUSER|CLONE_NEWUTS)==0);
  CHECK(writefile("/proc/self/uid_map","0 1000 1\n")==0);
  CHECK(writefile("/proc/self/setgroups","deny\n")==0);
  CHECK(writefile("/proc/self/gid_map","0 1000 1\n")==0);
  CHECK(getuid()==0&&geteuid()==0&&getgid()==0&&getegid()==0);
  CHECK(sethostname("rootless",8)==0);
  CHECK(setuid(1)<0&&errno==EINVAL);
  CHECK(setgroups(0,0)<0&&errno==EPERM);
  CHECK(open("/root/.profile",O_RDONLY)<0);
  int parent=open("/proc/1/ns/user",O_RDONLY);CHECK(parent>=0);
  if(parent>=0){CHECK(setns(parent,CLONE_NEWUSER)<0&&errno==EPERM);close(parent);}
  _exit(failures?1:0);
 }
 CHECK(waitgood(c));gethostname(now,sizeof(now));CHECK(!strcmp(now,host));
 semctl(sem,0,IPC_RMID);shmctl(shm,IPC_RMID,0);msgctl(mq,IPC_RMID,0);
 printf("USER_IPC_ISOLATION failures=%d\n",failures);return failures!=0;
}
