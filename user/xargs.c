#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: xargs <params>\n");
        exit(1);
    }

    char* child_argv[MAXARG];
    char buf[512] = {'\0'};
    int index = 0;
    for (int i = 1; i < argc; i++) {
        child_argv[index++] = argv[i];
    }

    sleep(10);

    // 从标准输入中读取命令到buf中
    // 若执行 echo 1 2 3，则标准输入为 1 2 3
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        char* p = buf;
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n')
                continue;
            if (fork() == 0) {
                buf[i] = '\0';
                // 例如：echo 1 | xargs echo 2
                // 在xargs中，标准输入为1，buf中内容为"1\n"，child_argv为["echo", "2"]，index为2
                // 程序执行到此处时，buf中内容变为了"1\0"，child_argv变为了["echo", "2", "1"]
                child_argv[index] = p;
                // exec的第一个参数是要执行的可执行文件的路径
                // 第二个参数是作为新命令的参数数组，数组的第一项为新命令的名称
                exec(child_argv[0], child_argv);
                exit(0);
            } else {
                // 在父进程中，跳过buf中的\n后，是一条新命令的开始
                p = &buf[i + 1];
                wait(0);
            }
        }
    }

    exit(0);
}
