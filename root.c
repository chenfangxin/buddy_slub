#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "rte_buddy.h"
#include "rte_slub.h"
#include "rte_mem.h"

#define HUGE_PAGE_DIR  "/dev/hugepages"
#define HUGE_PAGE_FILE "%s/.rte_maps_file"
#define HUGE_PAGE_SIZE 0x200000
#define RTE_SHM_FIXED_ADDR 0x100000000UL

/*
 * 内存控制结构
 * */
struct mem_cb{
	struct rte_mem_zone zone;
	struct rte_mem_cache mem_cache[RTE_SHM_CACHE_NUM];
	struct rte_page page[0];
};

static struct mem_cb *global_mem_cb=NULL;
static int mem_block_init(void)
{
	char filename[128];
	int fd=0;
	void *virtaddr=NULL;
	int ret=0;
	int mem_cb_len=0;

	mem_cb_len = sizeof(struct mem_cb) + 512*sizeof(struct rte_page);
	printf("mem_cb_len = %d\n", mem_cb_len);

	global_mem_cb = (struct mem_cb *)malloc(mem_cb_len);
	if(NULL==global_mem_cb){
		return -1;
	}

	// 分配一个hugepage内存, 作为总的内存池
	memset(filename, 0, 128);	
	sprintf(filename, HUGE_PAGE_FILE, HUGE_PAGE_DIR);
	fd = open(filename, O_CREAT|O_RDWR|O_TRUNC, 0777); //在hugetlbfs所挂载的目录中新建一个文件，即分配了一块大页内存
	if(fd<0){
		return -1;
	}

	virtaddr = mmap((void *)RTE_SHM_FIXED_ADDR, HUGE_PAGE_SIZE, (PROT_READ|PROT_WRITE), (MAP_FIXED|MAP_SHARED), fd, 0); //将大页影射到用户空间
	if(virtaddr==MAP_FAILED){
		perror("mmap");
		close(fd);
		return -1;
	}
	memset(virtaddr, 0, 0x200000); //每个大页2MB
	close(fd); // 进行了mmap影射后，可以关闭文件

	/* 一个大页(2MB)分为512小页，每个小页4KB,交给Buddy系统管理 */
	ret = rte_buddy_system_init(&global_mem_cb->zone, (unsigned long)virtaddr,global_mem_cb->page, 512);
	if(ret<0){
		goto out;
	}

	ret = rte_slub_system_init(global_mem_cb->mem_cache, RTE_SHM_CACHE_NUM);
	if(ret<0){
		goto out;
	}

out:
	return 0;	
}

int main(void)
{
	int ret=0;	
	void *ptr=NULL;

	ret = mem_block_init();
	if(ret<0){
		return -1;
	}
	ptr = rte_malloc(24);
	if(NULL==ptr){
		printf("Failed to malloc.\n");
	}
	printf("ptr = %p\n", ptr);	
	rte_free(ptr);

	return 0;
}
