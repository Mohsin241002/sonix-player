#include <linux/input.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
struct ev { int sec, usec; unsigned short type, code; int value; };
static int fd;
static void E(int t, int c, int v) { struct ev e = {0, 0, t, c, v}; write(fd, &e, sizeof(e)); }
int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "grab")) {
		int f = open("/dev/fb0", O_RDONLY); struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
		if (f < 0 || ioctl(f, FBIOGET_VSCREENINFO, &v) || ioctl(f, FBIOGET_FSCREENINFO, &x)) return 1;
		void *m = mmap(NULL, x.smem_len, PROT_READ, MAP_SHARED, f, 0); if (m == MAP_FAILED) return 2;
		FILE *o = fopen(argv[2], "wb"); if (!o) return 3;
		fwrite((char *)m + v.yoffset * x.line_length, 1, v.yres * x.line_length, o); fclose(o); return 0;
	}
	if (argc < 3) return 5;
	fd = open(argv[2], O_WRONLY); if (fd < 0) return 4;
	if (!strcmp(argv[1], "key") && argc == 4) {
		for (int d = 1; d >= 0; d--) { E(EV_KEY, atoi(argv[3]), d); E(EV_SYN, SYN_REPORT, 0); usleep(100000); }
		return 0;
	}
	if (argc < 5) return 5;
	int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = argc > 6 ? atoi(argv[5]) : x0, y1 = argc > 6 ? atoi(argv[6]) : y0;
	int steps = !strcmp(argv[1], "swipe") ? 20 : 1;
	E(EV_ABS, ABS_MT_SLOT, 0); E(EV_ABS, ABS_MT_TRACKING_ID, 777); E(EV_KEY, BTN_TOUCH, 1);
	for (int n = 0; n <= steps; n++) {
		E(EV_ABS, ABS_MT_POSITION_X, x0 + (x1 - x0) * n / steps); E(EV_ABS, ABS_MT_POSITION_Y, y0 + (y1 - y0) * n / steps);
		E(EV_SYN, SYN_REPORT, 0); usleep(16000);
	}
	usleep(!strcmp(argv[1], "hold") ? 850000 : 100000);
	E(EV_ABS, ABS_MT_TRACKING_ID, -1); E(EV_KEY, BTN_TOUCH, 0); E(EV_SYN, SYN_REPORT, 0);
	return 0;
}
