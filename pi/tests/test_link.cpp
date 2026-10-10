// SerialLink tested over a pseudo-terminal (no hardware needed).
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include "../src/link.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr(int fd, const uint8_t* b, int n) { if (write(fd, b, n) != n) perror("write"); }

int main() {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(master); unlockpt(master);
    std::string slave = ptsname(master);
    termios mt; tcgetattr(master, &mt); cfmakeraw(&mt); tcsetattr(master, TCSANOW, &mt);

    SerialLink link;
    CHECK(link.open(slave), "open: %s", link.error().c_str());

    Frame f;
    CHECK(!link.poll(f, 0), "empty port should give no frame");

    // 1. one clean frame
    uint8_t p1[1] = {2};
    uint8_t buf[16]; uint8_t n = build_frame(CMD_BUTTON, p1, 1, buf);
    wr(master, buf, n); usleep(20000);
    CHECK(link.poll(f, 0) && f.cmd == CMD_BUTTON && f.len == 1 && f.payload[0] == 2, "clean frame");
    CHECK(!link.poll(f, 0), "no extra frame");

    // 2. garbage + bad checksum + a good frame: parser must resync on the good one
    uint8_t junk[] = {0x00, 0x13, 0xAA, 0x01, 0x00, 0xFF, 0x55};  // AA 01 00 <bad cs> 55
    wr(master, junk, sizeof(junk));
    uint8_t pd[1] = {4}; n = build_frame(CMD_DONE, pd, 1, buf);
    wr(master, buf, n); usleep(20000);
    CHECK(link.poll(f, 0) && f.cmd == CMD_DONE && f.payload[0] == 4, "resync after garbage");
    CHECK(!link.poll(f, 0), "bad frame was dropped, not delivered");

    // 3. two frames back to back in one read: both delivered, in order
    uint8_t two[32]; int m = 0;
    m += build_frame(CMD_READY, nullptr, 0, two + m);
    uint8_t pe[1] = {1}; m += build_frame(CMD_ERROR, pe, 1, two + m);
    wr(master, two, m); usleep(20000);
    CHECK(link.poll(f, 0) && f.cmd == CMD_READY && f.len == 0, "first of two");
    CHECK(link.poll(f, 0) && f.cmd == CMD_ERROR && f.payload[0] == 1, "second of two");

    // 4. a frame split across two writes
    n = build_frame(CMD_DONE, pd, 1, buf);
    wr(master, buf, 3); usleep(20000);
    CHECK(!link.poll(f, 0), "partial frame must not deliver");
    wr(master, buf + 3, n - 3); usleep(20000);
    CHECK(link.poll(f, 0) && f.cmd == CMD_DONE, "split frame completes");

    // 5. Pi -> ESP32 direction: exact bytes on the wire
    uint8_t dp[2] = {5, 1};
    link.send(CMD_DROP, dp, 2);
    usleep(20000);
    uint8_t got[16]; ssize_t g = read(master, got, sizeof(got));
    uint8_t expect[16]; uint8_t en = build_frame(CMD_DROP, dp, 2, expect);
    CHECK(g == en, "wire length %zd vs %d", g, en);
    bool same = (g == en); for (int i = 0; same && i < en; i++) same = got[i] == expect[i];
    CHECK(same, "wire bytes");
    // spell it out once: AA 02 02 05 01 <cs> 55, cs = 02^02^05^01 = 04
    CHECK(got[0] == 0xAA && got[1] == 0x02 && got[2] == 0x02 && got[3] == 5 && got[4] == 1 && got[5] == 0x04 && got[6] == 0x55,
          "hand-computed DROP frame");

    // 6. open failure reports why
    SerialLink bad;
    CHECK(!bad.open("/dev/does_not_exist") && !bad.error().empty(), "bad device");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
