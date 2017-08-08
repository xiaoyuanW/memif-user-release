#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>

#include "../emif.h"
#include "../cmem-xzl.h"
#include "../edma3-xzl.h"
#include "../log.h"

void emif_perfcnt_start(void);
void emif_perfcnt_end(void);
int emif_perfcnt_init(void);

#define N 	64 * 1024
volatile uint32_t array[N];

int main(int argc, char **argv)
{
	int i;

	emif_perfcnt_init();

	emif_perfcnt_start();

	for (i = 0; i < N; i++)
		array[i] = 1 - i;

	emif_perfcnt_end();

	return 0;
}
