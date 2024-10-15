#include <sys/ioctl.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define COMENTO_DEVICE_NAME	"comento-mmio0"
#define COMENTO_MAGIC      	'C'
#define COMENTO_IOCTL_REMOVE	_IO(COMENTO_MAGIC, 0)

int main(int argc, char* argv[]) {
	int fd = open("/dev/" COMENTO_DEVICE_NAME, O_RDWR);
	if(fd < 0) {
		printf("Failed to open device\n");
		return -1;
	}

	if(ioctl(fd, COMENTO_IOCTL_REMOVE, 0) < 0) {
		printf("Failed to do ioctl command\n");
		return -1;
	}
	return 0;
}
