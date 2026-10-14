/* ashmemprobe — 宿主上对 /dev/ashmem(src/sandbox/phone/ashmem 树外模块)的用户态回归: 加载模块后先跑它, 全 OK 才让手机用。
 * 覆盖 UC 内核崩溃那条路(ASHMEM_GET_PROT_MASK / SET_PROT_MASK 与 mmap 权限匹配)和 ashmem 的全部 ioctl: 名字/大小/保护位/pin·unpin/purge。
 * 编: cc -O2 -o tests/phone/ashmemprobe tests/phone/ashmemprobe.c    跑: tests/phone/ashmemprobe   (退出码 0 = 全过; 每项一行 OK/FAIL)
 * 出错会怎样: /dev/ashmem 不在 → "open: No such file" (模块没载); 某项 FAIL → 模块移植有问题, 别给手机用。 */
#include "../../src/sandbox/phone/ashmem/ashmem_uapi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (cond) printf("OK   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int region(size_t size, const char *name) {
    int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/ashmem"); exit(2); }
    if (ioctl(fd, ASHMEM_SET_NAME, name) < 0) { perror("SET_NAME"); exit(2); }
    if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0) { perror("SET_SIZE"); exit(2); }
    return fd;
}

int main(void) {
    const size_t sz = 1 << 20;
    char name[ASHMEM_NAME_LEN] = "";
    int fd = region(sz, "ds4-probe");
    CHECK(ioctl(fd, ASHMEM_GET_NAME, name) == 0 && !strcmp(name, "ds4-probe"), "GET_NAME = %s", name);
    CHECK((size_t)ioctl(fd, ASHMEM_GET_SIZE) == sz, "GET_SIZE = %zu", sz);
    const int prot = ioctl(fd, ASHMEM_GET_PROT_MASK);
    CHECK(prot == (PROT_READ | PROT_WRITE | PROT_EXEC), "GET_PROT_MASK 默认 = rwx (0x%x)", prot);   /* Chromium 崩在这一问: ioctl 失败回 -1 */

    /* 共享映射: 写进去, fork 的子进程读得到 */
    unsigned char *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "mmap MAP_SHARED rw");
    if (m == MAP_FAILED) return 1;
    memset(m, 0x5a, sz); m[sz - 1] = 0xa5;
    pid_t pid = fork();
    if (pid == 0) { _exit(m[0] == 0x5a && m[sz - 1] == 0xa5 ? 0 : 1); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "子进程看到同一份数据");
    /* 第二个映射看同一页(两次 mmap 同一 fd 共享后备文件) */
    unsigned char *m2 = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(m2 != MAP_FAILED && m2[0] == 0x5a && m2[sz - 1] == 0xa5, "第二次 mmap 读到同一份");
    if (m2 != MAP_FAILED) munmap(m2, sz);

    /* pin / unpin / purge: unpin 的页可被回收, PURGE_ALL_CACHES(要 CAP_SYS_ADMIN, 普通用户 EPERM 也算符合) 后 pin 回报 WAS_PURGED */
    struct ashmem_pin pin = { .offset = 0, .len = 4096 };
    CHECK(ioctl(fd, ASHMEM_GET_PIN_STATUS) == ASHMEM_IS_PINNED, "初始 PIN_STATUS = pinned");
    CHECK(ioctl(fd, ASHMEM_UNPIN, &pin) == ASHMEM_IS_UNPINNED, "UNPIN 第一页");
    CHECK(ioctl(fd, ASHMEM_GET_PIN_STATUS) == ASHMEM_IS_UNPINNED, "PIN_STATUS = unpinned");
    const int purge = ioctl(fd, ASHMEM_PURGE_ALL_CACHES);
    const int repin = ioctl(fd, ASHMEM_PIN, &pin);
    CHECK((purge >= 0 && repin == ASHMEM_WAS_PURGED) || (purge < 0 && errno == EPERM && repin == ASHMEM_NOT_PURGED), "PURGE(%s) → PIN 回 %s", purge >= 0 ? "root 执行" : "非 root EPERM", repin == ASHMEM_WAS_PURGED ? "WAS_PURGED" : "NOT_PURGED");
    CHECK(ioctl(fd, ASHMEM_GET_PIN_STATUS) == ASHMEM_IS_PINNED, "重新 pinned");
    munmap(m, sz);

    /* 保护位: 先 SET_PROT_MASK 只读, 再 mmap 可写 → EPERM; 只读 mmap 可以; 之后想放宽回可写 → EINVAL(只许收紧) */
    int ro = region(4096, "ds4-ro");
    CHECK(ioctl(ro, ASHMEM_SET_PROT_MASK, PROT_READ) == 0, "SET_PROT_MASK 只读");
    CHECK(ioctl(ro, ASHMEM_GET_PROT_MASK) == PROT_READ, "GET_PROT_MASK = r");
    void *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
    CHECK(w == MAP_FAILED && errno == EPERM, "只读区 mmap rw → EPERM");
    void *r = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
    CHECK(r != MAP_FAILED, "只读区 mmap r 可以");
    if (r != MAP_FAILED) munmap(r, 4096);
    CHECK(ioctl(ro, ASHMEM_SET_PROT_MASK, PROT_READ | PROT_WRITE) < 0 && errno == EINVAL, "放宽保护位 → EINVAL");
    /* mmap 之后不许再改大小 */
    CHECK(ioctl(fd, ASHMEM_SET_SIZE, 8192) < 0 && errno == EINVAL, "映射过的区改 SET_SIZE → EINVAL");
    close(ro); close(fd);

    /* 关 fd 后映射还活着(后备 shmem 文件被 vma 引用) */
    int f3 = region(4096, "ds4-close");
    unsigned char *m3 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f3, 0);
    close(f3);
    CHECK(m3 != MAP_FAILED && (m3[0] = 7) == 7 && m3[0] == 7, "close 后映射仍可读写");
    if (m3 != MAP_FAILED) munmap(m3, 4096);

    printf("%s: %d fail\n", fails ? "FAIL" : "ALL OK", fails);
    return fails ? 1 : 0;
}
