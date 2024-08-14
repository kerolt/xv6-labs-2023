#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char* GetName(char* path) {
    char* p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--) {}
    p++;
    return p;
}

void Find(char* dir, char* name) {
    char buf[512];
    char* p;
    int fd = open(dir, 0);
    struct dirent de;
    struct stat st;

    if (fd < 0) {
        printf("Error: open\n");
        return;
    }
    if (fstat(fd, &st) < 0) {
        printf("Error: stat\n");
        close(fd);
        return;
    }
    if (st.type != T_DIR) {
        printf("the current the file is not dictionary\n", dir);
        close(fd);
        return;
    }

    strcpy(buf, dir);
    p = buf + strlen(buf);
    *p++ = '/';

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0)
            continue;
        if (strcmp(de.name, ".") == 0)
            continue;
        if (strcmp(de.name, "..") == 0)
            continue;

        char* cur = p;
        memmove(cur, de.name, DIRSIZ);
        cur[DIRSIZ] = 0;

        if (stat(buf, &st) < 0) {
            printf("Error: stat\n");
            continue;
        }

        switch (st.type) {
            case T_FILE:
                if (strcmp(GetName(buf), name) == 0) {
                    printf("%s\n", buf);
                }
                break;
            case T_DIR:
                if (strlen(dir) + 1 + DIRSIZ + 1 > sizeof(buf)) {
                    printf("Error: path too long\n");
                    break;
                }
                Find(buf, name);
                break;
        }
    }

    close(fd);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: find <dir> <file_name>\n");
        exit(1);
    }
    Find(argv[1], argv[2]);
    exit(0);
}
