#define _POSIX_C_SOURCE 200809L
#include "../src/tune_control.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
static void send_text(int fd, const char *s) { assert(write(fd, s, strlen(s)) == (ssize_t)strlen(s)); }
int main(void) {
    int fds[2]; assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    TuneControl control = {0}; double frequency = 28000000, gain = 10, bandwidth = 0;
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    send_text(fds[1], "28000");
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    send_text(fds[1], "100\n28000200\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    assert(frequency == 28000200);
    send_text(fds[1], "nan\ninf\n436000000\n499\n28000000oops\n\n");
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    char long_line[100]; memset(long_line, '1', sizeof(long_line)-1); long_line[99] = 0;
    send_text(fds[1], long_line); send_text(fds[1], "\n500\n30000000\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    assert(frequency == 30000000);
    send_text(fds[1], "gain -10\n28000100\ngain -20\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth) == 3);
    assert(gain == -20 && frequency == 28000100);
    send_text(fds[1], "gain 5\ngain -30\ngain 20\ngain nan\ngain nope\n");
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    assert(gain == -20);
    send_text(fds[1], "gain 9");
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, 0, 76, 0, &gain, 111111, &bandwidth));
    send_text(fds[1], "\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, 0, 76, 0, &gain, 111111, &bandwidth) == 2);
    assert(gain == 9);
    send_text(fds[1], "bandwidth 40000\ngain 0\n28000000\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth) == 7);
    assert(bandwidth == 40000 && gain == 0 && frequency == 28000000);
    send_text(fds[1], "bandwidth -1\nbandwidth 1\nbandwidth 111111\nbandwidth nan\n");
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    send_text(fds[1], "bandwidth 0\n");
    assert(tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth) == 4);
    assert(bandwidth == 0);
    close(fds[1]);
    assert(!tune_control_read(&control, fds[0], 500, 30000000, &frequency, -20, 10, 10, &gain, 111111, &bandwidth));
    close(fds[0]);
    puts("Tuning control: partial commands, latest request, invalid values, boundaries and EOF passed");
    return 0;
}
