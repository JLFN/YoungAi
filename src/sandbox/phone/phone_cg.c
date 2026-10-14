/* phone_cg.c — 手机的 cgroup v2 子树 /sys/fs/cgroup/ds4-phone: 内存硬顶、进程数上限、CPU 配额、整树击杀。
 * 为什么用 cgroup 而不记 pid: Android 的 init 起来后 fork 出几十个进程, 杀 init 不保证子孙都走; cgroup.kill 一次把整棵树清掉。
 * memory.swap.max = 0: 手机不许用交换 —— 顶是真顶, 超了就 OOM, 不许偷偷把模型子进程的页挤到盘上。
 * 出错会怎样: 根 cgroup 的 subtree_control 没放 memory/pids/cpu → 写 memory.max 报 ENOENT; spark 上 10-13 查过是放了的(cpuset cpu io memory pids)。 */
#include "phone_internal.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static bool put(const char *file, const char *val) {
    char p[256]; snprintf(p, sizeof p, PH_CGROUP "/%s", file);
    FILE *f = fopen(p, "w"); if (!f) return false;
    const bool ok = fputs(val, f) >= 0; return fclose(f) == 0 && ok;
}

static long get_num(const char *file) {
    char p[256]; snprintf(p, sizeof p, PH_CGROUP "/%s", file);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    long v = -1; if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); return v;
}

bool ph_cg_exists(void) { struct stat st; return stat(PH_CGROUP, &st) == 0 && S_ISDIR(st.st_mode); }

static long mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1;
    char line[256]; long kb = -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtol(line + 13, NULL, 10); break; }
    fclose(f); return kb < 0 ? -1 : kb / 1024;
}

bool ph_cg_create(char *err, size_t errn) {
    /* 顶按当时的账算(phone_internal.h 的说明): 宿主留 PH_MEM_HEADROOM_MB, 其余给手机, 封在 [MIN, MAX] */
    const long avail = mem_avail_mb();
    long cap = avail < 0 ? PH_MEM_MIN_MB : avail - PH_MEM_HEADROOM_MB;
    if (cap > PH_MEM_MAX_MB) cap = PH_MEM_MAX_MB;
    if (cap < PH_MEM_MIN_MB) { snprintf(err, errn, "内存不够: 宿主可用 %ld MB, 手机至少要 %d MB + 给宿主留 %d MB", avail, PH_MEM_MIN_MB, PH_MEM_HEADROOM_MB); return false; }
    if (mkdir(PH_CGROUP, 0755) && errno != EEXIST) { snprintf(err, errn, "mkdir %s: %s", PH_CGROUP, strerror(errno)); return false; }
    char v[64];
    snprintf(v, sizeof v, "%lld", (long long)cap * 1048576);
    if (!put("memory.max", v)) { snprintf(err, errn, "memory.max: %s(根 cgroup 没放 memory 控制器?)", strerror(errno)); return false; }
    ph_log("内存顶 %ld MB(宿主可用 %ld MB − 留 %d MB, 封 %d..%d)", cap, avail, PH_MEM_HEADROOM_MB, PH_MEM_MIN_MB, PH_MEM_MAX_MB);
    if (!put("memory.swap.max", "0")) { snprintf(err, errn, "memory.swap.max: %s", strerror(errno)); return false; }
    snprintf(v, sizeof v, "%d", PH_PIDS_MAX);
    if (!put("pids.max", v)) { snprintf(err, errn, "pids.max: %s", strerror(errno)); return false; }
    snprintf(v, sizeof v, "%d 100000", PH_CPU_CORES * 100000);
    if (!put("cpu.max", v)) { snprintf(err, errn, "cpu.max: %s", strerror(errno)); return false; }
    return true;
}

bool ph_cg_join(pid_t pid) { char v[32]; snprintf(v, sizeof v, "%ld", (long)pid); return put("cgroup.procs", v); }

/* 递归: Android 的 init 在自己的 cgroup 命名空间里建子 cgroup(ds4-phone/<它的层级>), 进程散在各层, 只看顶层 cgroup.procs 会以为手机只有两个进程(10-13 实撞) */
static int count_tree(const char *dir, long *lo, int depth) {
    char p[512]; snprintf(p, sizeof p, "%s/cgroup.procs", dir);
    int n = 0; long pid;
    FILE *f = fopen(p, "r");
    if (f) { while (fscanf(f, "%ld", &pid) == 1) { n++; if (!*lo || pid < *lo) *lo = pid; } fclose(f); }
    if (depth > 8) return n;
    DIR *d = opendir(dir); if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_type == DT_DIR && e->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); n += count_tree(p, lo, depth + 1); }
    closedir(d);
    return n;
}

int ph_cg_pids(pid_t *first) {
    long lo = 0; const int n = count_tree(PH_CGROUP, &lo, 0);
    if (first) *first = (pid_t)lo;
    return n;
}

static void rmdir_tree(const char *dir, int depth) {   /* 子 cgroup 要先于父删(深的先), 空了才删得掉 */
    if (depth > 8) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char p[1536];
    while ((e = readdir(d))) if (e->d_type == DT_DIR && e->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); rmdir_tree(p, depth + 1); rmdir(p); }
    closedir(d);
}

long ph_cg_mem_mb(bool peak) { const long b = get_num(peak ? "memory.peak" : "memory.current"); return b < 0 ? -1 : b / 1048576; }
long ph_cg_cap_mb(void) { const long b = get_num("memory.max"); return b < 0 ? -1 : b / 1048576; }

bool ph_cg_kill(void) {
    if (!ph_cg_exists()) return true;
    put("cgroup.kill", "1");   /* 5.14+ 的一次性整树 SIGKILL; 没有这个文件的老内核会走下面的空等然后 rmdir 失败 */
    for (int i = 0; i < PH_STOP_WAIT_S * 10 && ph_cg_pids(NULL) > 0; i++) usleep(100000);
    if (ph_cg_pids(NULL) > 0) { ph_log("cgroup.kill 后 %d s 还有进程", PH_STOP_WAIT_S); return false; }
    /* 进程表空了之后内核还要一会儿才把它们从子 cgroup 里摘干净, rmdir 这段时间报 EBUSY(10-13 实撞), 所以重试几秒 */
    for (int i = 0; i < 30; i++) { rmdir_tree(PH_CGROUP, 0); if (!rmdir(PH_CGROUP) || errno == ENOENT) return true; usleep(100000); }
    ph_log("rmdir %s: %s", PH_CGROUP, strerror(errno));
    return false;
}
