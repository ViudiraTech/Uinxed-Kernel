#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
static unsigned long failures;
static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void *worker(void *arg) {
    (void)arg;
    for(unsigned i=0;i<500000;i++) {
        struct timespec a,b,c;
        long x=syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&a);
        int y=clock_gettime(CLOCK_MONOTONIC,&b);
        long z=syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&c);
        if(x||y||z||b.tv_nsec<0||b.tv_nsec>=1000000000||ns(b)<ns(a)-1000000||ns(b)>ns(c)+1000000) {
            unsigned long n=__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
            if(n<5) printf("BAD rc=%ld/%d/%ld times=%lld/%lld/%lld\n",x,y,z,(long long)ns(a),(long long)ns(b),(long long)ns(c));
        }
    }
    return 0;
}
int main(void) {
    pthread_t threads[4];
    for(int i=0;i<4;i++) if(pthread_create(&threads[i],0,worker,0)) return 2;
    for(int i=0;i<4;i++) pthread_join(threads[i],0);
    printf("VDSO_STRESS samples=2000000 failures=%lu\n",failures);
    return failures!=0;
}
