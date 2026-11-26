#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "edu.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>

#define reg32(base, offset) \
	(*(volatile __u32 *)((volatile char *)(base) + (offset)))

#define DMA_MSG "Hello, world!"
#define DMA_MSG_SIZE sizeof(DMA_MSG)

#define perror_return(msg, ret) \
	do {                    \
		perror(msg);    \
		return ret;     \
	} while (0)

#define perror_goto(msg, point) \
	do {                    \
		perror(msg);    \
		goto point;     \
	} while (0)

static bool edu_test_dma(int fd)
{
	size_t count;
	char buffer[4096];

	printf("/dev/edu0/dma testing...\n");

	count = (EDU_DMA_SIZE > sizeof(buffer)) ? sizeof(buffer) : EDU_DMA_SIZE;

	printf("fill buffer 'a' %zu symbols\n", count);

	memset(buffer, 'a', count);

	printf("write buffer to dma %zu bytes...\n", count);

	if (write(fd, buffer, count) < 0)
		perror_return("write", false);

	printf("zero out the buffer\n");

	memset(buffer, 0, count);

	printf("read dma to buffer %zu bytes\n", count - 1);

	if (pread(fd, buffer, count - 1, 0) < 0)
		perror_return("read", false);

	for (size_t i = 0; i < count - 1; i++) {
		if (buffer[i] != 'a' || (buffer[i] == '\0' && i != count - 1)) {
			printf("Invalid result!\n");
			return false;
		}
	}

	printf("/dev/edu0/dma test success!\n");

	return true;
}

static bool edu_test_factorial(int fd)
{
	__u32 val = 4;

	printf("/dev/edu0/fac testing...\n");

	if (write(fd, &val, 4) != 4)
		perror_return("write /dev/edu0/fac", false);

	if (read(fd, &val, 4) != 4)
		perror_return("read /dev/edu0/fac", false);

	printf("fac: 4! = %u\n", val);

	if (val != 24) {
		printf("Invalid result!\n");
		return false;
	}

	printf("/dev/edu0/fac test success!\n");

	return true;
}

static bool edu_test_mmio(int fd)
{
	volatile void *mmio;
	__u32 size;
	__u32 res;
	bool ret = false;

	printf("/dev/edu0/mmio testing...\n");

	if (ioctl(fd, EDU_MMIO_SIZE, &size))
		perror_return("ioctl EDU_MMIO_SIZE", false);

	mmio = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (mmio == MAP_FAILED)
		perror_return("mmap mmio", false);

	printf("we begin calculating the factorial of 5\n");

	reg32(mmio, EDU_FAC_COMP_REG) = 5;

	printf("awaiting the result...\n");

	while (reg32(mmio, EDU_STATUS_REG) & 0x01)
		;

	res = reg32(mmio, EDU_FAC_COMP_REG);

	printf("fac (mmio test): !5 = %d\n", res);

	if (res != 120) {
		printf("Invalid result!\n");
		goto out;
	}

	ret = true;
	printf("/dev/edu0/mmio test success!\n");
out:
	munmap((void *)mmio, size);
	return ret;
}

int main(void)
{
	int ret = 1;
	int dma, fac, mmio = -1;
	unsigned int success = 0;

	dma = open("/dev/edu0/dma", O_RDWR);

	if (dma < 0)
		perror_return("open /dev/edu0/dma", 1);

	fac = open("/dev/edu0/fac", O_RDWR);

	if (fac < 0)
		perror_goto("open /dev/edu0/fac", cleanup);

	mmio = open("/dev/edu0/mmio", O_RDWR);

	if (mmio < 0)
		perror_goto("open /dev/edu0/mmio", cleanup);

	success += edu_test_factorial(fac);
	success += edu_test_dma(dma);
	success += edu_test_mmio(mmio);

	printf("Tests passed successfully: %u out of 3\n", success);

	ret = 0;
cleanup:
	if (dma >= 0)
		close(dma);
	if (fac >= 0)
		close(fac);
	if (mmio >= 0)
		close(mmio);

	return ret;
}

