#include <stdio.h>
#include <unistd.h>

int main(void)
{

    char *argv[] = {
        "/sbin/init",
        NULL
    };

    char *envp[] = {
        NULL
    };

    if (execve("/sbin/init", argv, envp) != 0){
        printf("init startup error.\n");
    }
    printf("System paused!\n");
    pause();
}
