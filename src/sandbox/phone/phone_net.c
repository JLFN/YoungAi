/* phone_net.c — 手机的网络: 一对 veth(宿主 ds4ph0 ⇄ 手机 eth0, 172.31.250.0/30) + 宿主 NAT 出公网 + 内网隔断。
 * 为什么隔断: 默认情况下手机能摸到宿主所在的局域网(NAS、路由器后台), 这绕过了助理那道"本机/内网地址直接拒"的守门;
 *   所以 FORWARD 链上先 DROP 全部私网段(10/8 172.16/12 192.168/16 169.254/16), INPUT 链上只放手机对宿主已建立连接的回包
 *   (adb 是宿主主动连手机的 5555, 回包要进来; 手机主动连宿主的任何端口一律 DROP)。
 * 为什么 fork /usr/sbin/ip 和 iptables 而不写 rtnetlink: 两者是 Ubuntu 的基础包(iproute2 是 Essential), 不是守护进程, 和 modprobe 一个性质;
 *   裸 netlink 建 veth + 搬命名空间 + nft 规则要 400 行且每条都得自测, 换来的只是少 fork 两次。
 * DNS: spark 的 /etc/resolv.conf 指向 systemd-resolved 的 127.0.0.53, 手机那头 127.0.0.53 是它自己 —— 要给上游地址
 *   (/run/systemd/resolve/resolv.conf); 上游若在私网段(路由器当 DNS)就单独放行它的 53 口。
 * 出错会怎样: 残留的 ds4ph0 → start 前先 down 一遍(ip link del 顺带删掉手机那头); iptables -D 删不存在的规则报错, 一律忽略。 */
#include "phone_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static bool private_ip(const char *ip) {
    struct in_addr a; if (inet_pton(AF_INET, ip, &a) != 1) return false;
    const unsigned v = ntohl(a.s_addr);
    return (v >> 24) == 10 || (v >> 20) == (172u << 4 | 1) || (v >> 16) == (192u << 8 | 168) || (v >> 16) == (169u << 8 | 254) || (v >> 24) == 127;
}

void ph_net_dns(char *out, size_t n) {
    static const char *const files[] = { "/run/systemd/resolve/resolv.conf", "/etc/resolv.conf", NULL };
    snprintf(out, n, "8.8.8.8");
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "r"); if (!f) continue;
        char line[256], ip[64];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "nameserver %63s", ip) != 1) continue;
            if (!strncmp(ip, "127.", 4) || strchr(ip, ':')) continue;   /* 本机回环 / IPv6 都不给手机 */
            snprintf(out, n, "%s", ip); fclose(f); return;
        }
        fclose(f);
    }
}

static int ipt(const char *op, const char *table, const char *chain, char *const rule[]) {   /* iptables [-t table] op chain rule… */
    char *argv[32]; int k = 0;
    argv[k++] = "iptables"; argv[k++] = "-w"; argv[k++] = "2";
    if (table) { argv[k++] = "-t"; argv[k++] = (char *)table; }
    argv[k++] = (char *)op; argv[k++] = (char *)chain;
    for (int i = 0; rule[i] && k < 30; i++) argv[k++] = rule[i];
    argv[k] = NULL;
    return ph_run(argv, !strcmp(op, "-D"));
}

/* 规则表: 装用 -I/-A, 拆用 -D(同一份 spec, 顺序无所谓) */
static char *const R_NAT[]  = { "-s", PH_SUBNET, "!", "-o", PH_VETH_HOST, "-j", "MASQUERADE", NULL };
static char *const R_FWD_EST[] = { "-o", PH_VETH_HOST, "-m", "conntrack", "--ctstate", "RELATED,ESTABLISHED", "-j", "ACCEPT", NULL };
static char *const R_FWD_OUT[] = { "-i", PH_VETH_HOST, "-j", "ACCEPT", NULL };
static char *const R_FWD_DROP[][8] = {
    { "-i", PH_VETH_HOST, "-d", "10.0.0.0/8", "-j", "DROP", NULL },
    { "-i", PH_VETH_HOST, "-d", "172.16.0.0/12", "-j", "DROP", NULL },
    { "-i", PH_VETH_HOST, "-d", "192.168.0.0/16", "-j", "DROP", NULL },
    { "-i", PH_VETH_HOST, "-d", "169.254.0.0/16", "-j", "DROP", NULL },
};
static char *const R_IN_EST[] = { "-i", PH_VETH_HOST, "-m", "conntrack", "--ctstate", "RELATED,ESTABLISHED", "-j", "ACCEPT", NULL };
static char *const R_IN_DROP[] = { "-i", PH_VETH_HOST, "-j", "DROP", NULL };
static char g_dns[64];
static char *const R_FWD_DNS[] = { "-i", PH_VETH_HOST, "-d", g_dns, "-p", "udp", "--dport", "53", "-j", "ACCEPT", NULL };

static void rules(bool add) {
    const char *op = add ? "-I" : "-D";
    /* 插入顺序倒着来: -I 每次插到第 1 行, 最后插的最先匹配 ⇒ DROP 要最后插才排在 ACCEPT 前面; DNS 放行再后面(排在 DROP 前) */
    if (add) {
        ipt("-A", "nat", "POSTROUTING", R_NAT);
        ipt(op, NULL, "FORWARD", R_FWD_OUT); ipt(op, NULL, "FORWARD", R_FWD_EST);
        for (int i = 0; i < 4; i++) ipt(op, NULL, "FORWARD", R_FWD_DROP[i]);
        if (private_ip(g_dns)) ipt(op, NULL, "FORWARD", R_FWD_DNS);
        ipt(op, NULL, "INPUT", R_IN_DROP); ipt(op, NULL, "INPUT", R_IN_EST);
    } else {
        ipt(op, "nat", "POSTROUTING", R_NAT);
        ipt(op, NULL, "FORWARD", R_FWD_OUT); ipt(op, NULL, "FORWARD", R_FWD_EST);
        for (int i = 0; i < 4; i++) ipt(op, NULL, "FORWARD", R_FWD_DROP[i]);
        ipt(op, NULL, "FORWARD", R_FWD_DNS);
        ipt(op, NULL, "INPUT", R_IN_DROP); ipt(op, NULL, "INPUT", R_IN_EST);
    }
}

bool ph_net_up(pid_t phone_pid, char *err, size_t errn) {
    ph_net_dns(g_dns, sizeof g_dns);
    char pid[32]; snprintf(pid, sizeof pid, "%ld", (long)phone_pid);
    char *const add[] = { "ip", "link", "add", PH_VETH_HOST, "type", "veth", "peer", "name", "eth0", "netns", pid, NULL };
    if (ph_run(add, false)) { snprintf(err, errn, "ip link add veth 失败(iproute2 在? 命名空间 %s 还在?)", pid); return false; }
    char *const addr[] = { "ip", "addr", "add", PH_IP_HOST "/30", "dev", PH_VETH_HOST, NULL };
    char *const up[] = { "ip", "link", "set", PH_VETH_HOST, "up", NULL };
    if (ph_run(addr, false) || ph_run(up, false)) { snprintf(err, errn, "宿主侧 veth 配置失败"); return false; }
    FILE *f = fopen("/proc/sys/net/ipv4/ip_forward", "w"); if (f) { fputs("1\n", f); fclose(f); }
    rules(true);
    return true;
}

void ph_net_down(void) {
    char *const del[] = { "ip", "link", "del", PH_VETH_HOST, NULL };
    ph_run(del, true);
    if (!g_dns[0]) ph_net_dns(g_dns, sizeof g_dns);
    rules(false);
}

bool ph_net_phone_side(void) {
    char *const lo[] = { "ip", "link", "set", "lo", "up", NULL };
    char *const addr[] = { "ip", "addr", "add", PH_IP_PHONE "/30", "dev", "eth0", NULL };
    char *const up[] = { "ip", "link", "set", "eth0", "up", NULL };
    char *const rt[] = { "ip", "route", "add", "default", "via", PH_IP_HOST, "dev", "eth0", NULL };
    return !ph_run(lo, false) && !ph_run(addr, false) && !ph_run(up, false) && !ph_run(rt, false);
}
