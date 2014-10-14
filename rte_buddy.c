/*
 * */
#include <stdio.h>
#include <string.h>
#include "rte_buddy.h"

static struct rte_mem_zone *global_mem_zone; 

static inline int page_zone_id(struct rte_page *page)
{
	return 0;	
}

#define page_private(page) ((page)->private)
#define set_page_private(page, v) ((page)->private = (v))
static inline void set_page_order(struct rte_page *page, uint32_t order)
{
	set_page_private(page, order);
	__SetPageBuddy(page);
}

static inline void rmv_page_order(struct rte_page *page)
{
	__ClearPageBuddy(page);	
	set_page_private(page, 0);
}

static inline uint32_t page_order(struct rte_page *page)
{
	return page_private(page);
}

static inline struct rte_page *__page_find_buddy(struct rte_page *page, uint32_t page_idx, uint32_t order)
{
	uint32_t buddy_idx = page_idx ^(1<<order);
	return page + (buddy_idx - page_idx);
}

static inline int page_is_buddy(struct rte_page *page, struct rte_page *buddy, int order)
{
	if(page_zone_id(page)!=page_zone_id(buddy)){
		return 0;
	}
	if(PageBuddy(buddy) && (page_order(buddy)==order)){
		return 1;
	}
	return 0;
}

static inline uint32_t __find_combined_index(uint32_t page_idx, uint32_t order)
{
	return (page_idx &~(1U<<order));
}

static inline void set_compound_order(struct rte_page *page, unsigned int order)
{
	page[1].lru.prev = (void *)((unsigned long)order);
}

/* 设置组合页的属性 */
static void prepare_compound_page(struct rte_page *page, unsigned int order)
{
	unsigned int i;
	unsigned int nr_pages = (1<<order);

	set_compound_order(page, order); // 标记页的大小(order值)
	__SetPageHead(page); // 首页设置head标志
	for(i=1; i<nr_pages; i++){
		struct rte_page *p = page + i;
		__SetPageTail(p); // 其余页设置tail标志
		p->first_page = page;
	}
}

static int destroy_compound_page(struct rte_page *page, unsigned int order)
{
	unsigned int i;
	unsigned int nr_pages = (1<<order);
	int bad = 0;
	__ClearPageHead(page);
	for(i=1;i<nr_pages;i++){
		struct rte_page *p = page + i;
		if(unlikely(!PageTail(p))||(p->first_page != page)){
			bad++;
			RTE_BUDDY_BUG(__FILE__, __LINE__);
		}
		__ClearPageTail(p);
	}
	return bad;

}

/*
 * expand函数用于将组合页进行分裂，以获得所需要大小的页
 * 参数：
 * 	page: 指向组合页首页的描述符。组合页可视为页的数组
 *	low: 目标页的大小(order值)
 *	high: 要分裂的组合页的大小(order值)
 * */
static inline void expand(struct rte_mem_zone *zone, struct rte_page *page,
				unsigned int low, unsigned int high, struct free_area *area)
{
	unsigned int size=(1U<<high);

	while(high>low){
		area--;
		high--;
		size >>= 1;
		list_add(&page[size].lru, &area->free_list);
		area->nr_free++;
		set_page_order(&page[size], high);
	}
}

static struct rte_page *__alloc_page(unsigned int order, struct rte_mem_zone *zone)
{
	struct rte_page *page=NULL;
	struct free_area *area=NULL;
	unsigned int current_order=0;

	for(current_order=order; current_order<RTE_MAX_ORDER; current_order++){
		area = zone->free_area + current_order;
		if(list_empty(&area->free_list)){
			continue;
		}
		page = list_entry(area->free_list.next, struct rte_page, lru);
		list_del(&page->lru);
		rmv_page_order(page);
		area->nr_free--;
		expand(zone, page, order, current_order, area);
		if(page && order){
			prepare_compound_page(page, order);
		}
		zone->free_zero_num -= (1<<order);
		return page;
	}
	return NULL;
}

struct rte_page *rte_get_pages(unsigned int order)
{
	struct rte_page *page = NULL;
	struct rte_mem_zone *zone = global_mem_zone;

	if(order>=RTE_MAX_ORDER){
		RTE_BUDDY_BUG(__FILE__, __LINE__);
		return NULL;
	}
	rte_spinlock_lock(&zone->lock);
	page = __alloc_page(order, zone);
	rte_spinlock_unlock(&zone->lock);
	return page;
}

void rte_free_pages(struct rte_page *page)
{
	struct rte_mem_zone *zone = global_mem_zone;	
	uint32_t page_idx = (page - zone->first_page);
	uint32_t order = compound_order(page);

	rte_spinlock_lock(&zone->lock);
	if(unlikely(PageCompound(page))){
		if(unlikely(destroy_compound_page(page, order))){
			RTE_BUDDY_BUG(__FILE__, __LINE__);
		}
	}

	zone->free_zero_num += (1<<order);	
	while(order<RTE_MAX_ORDER-1){
		uint32_t combinded_idx;
		struct rte_page *buddy;
		buddy = __page_find_buddy(page, page_idx, order);
		if(!page_is_buddy(page, buddy, order)){
			break;
		}
		list_del(&buddy->lru);
		zone->free_area[order].nr_free--;
		rmv_page_order(buddy);
		combinded_idx = __find_combined_index(page_idx, order);
		page = page + (combinded_idx - page_idx);
		page_idx = combinded_idx;
		order++;
	}

	set_page_order(page, order);
	list_add(&page->lru, &zone->free_area[order].free_list);
	zone->free_area[order].nr_free++;
	rte_spinlock_unlock(&zone->lock);
	return;
}

int rte_buddy_system_init(struct rte_mem_zone *zone, unsigned long start_addr, 
						  struct rte_page *start_page, unsigned int page_size, unsigned int page_num)
{
	struct rte_page *page=NULL;
	unsigned int i;
	struct free_area *area=NULL;;

	global_mem_zone = zone;

	// Init mem zone	
	rte_spinlock_init(&zone->lock);
	for(i=0; i<RTE_MAX_ORDER; i++){
		area = zone->free_area + i;
		INIT_LIST_HEAD(&area->free_list);
		area->nr_free = 0;
	}
	zone->page_num = page_num;
	zone->page_size = page_size;
	zone->first_page = start_page;
	zone->start_addr = start_addr;
	zone->end_addr = zone->start_addr + (page_num * page_size);

	for(i=0; i<page_num; i++){
		page = zone->first_page + i;
		memset(page, 0, sizeof(struct rte_page));
		INIT_LIST_HEAD(&page->lru);
		rte_spinlock_init(&page->lock);
		rte_free_pages(page);
	}

	return 0;
}

void *rte_page_to_virt(struct rte_page *page)
{
	uint32_t page_idx=0;
	uint64_t address=0;
	struct rte_mem_zone *zone=global_mem_zone;

	page_idx = page - zone->first_page;
	address = zone->start_addr + page_idx * RTE_PAGE_SIZE;

	return (void *)address;
}

struct rte_page *rte_virt_to_page(void *ptr)
{
	uint32_t page_idx=0;
	struct rte_mem_zone *zone=global_mem_zone;
	struct rte_page *page=NULL;
	uint64_t address=(uint64_t)ptr;
	
	if((address<zone->start_addr)||(address>zone->end_addr)){
		printf("start_addr=0x%lx, end_addr=0x%lx, address=0x%lx\n", 
				zone->start_addr, zone->end_addr, address);

		RTE_BUDDY_BUG(__FILE__, __LINE__);
	}
	page_idx = (address - zone->start_addr)>>RTE_PAGE_SHIFT;

	page = zone->first_page + page_idx;
	return page;
}

static inline struct rte_page *compound_head(struct rte_page *page)
{
	if(unlikely(PageTail(page))){
		return page->first_page;
	}
	return page;
}

struct rte_page *rte_virt_to_head_page(void *ptr)
{
	struct rte_page *page = rte_virt_to_page(ptr);
	return compound_head(page);
}

