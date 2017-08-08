#include "mig.h"
#include <sys/mman.h>
#include <stdio.h>

#define PAGE_COUNT 1024
//#define PAGE_SIZE 4096

/* allocate the pages that back the migregion.*/
void *alloc_memory(uint32_t pagecount){
	void *p;
	p = mmap(0, pagecount * PAGE_SIZE,
	         (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(!p){
		perror("mmap");
		return NULL;
	}
	printf("mmap'd %d pages okay\n", pagecount);
	return p;
}


int main(){
	void *p;
	int ret;
	mig_region * usr_region;
	mig_desc * base;

	/* alloc memory*/
	p = alloc_memory(PAGE_COUNT);
	
	/* init imgration*/
	usr_region = init_migregion(p, PAGE_SIZE * PAGE_COUNT);
	printf("ndescs = %d\n", usr_region->ndescs);


cleanup:
	ret = munmap(p, PAGE_COUNT * PAGE_SIZE);
	if(ret){
		printf("munmap failed!\n");
	}else{
		printf("unmap success!\n");
	}
	

	return 0;
}
