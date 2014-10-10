#ifndef __RTE_BUDDY_H__
#define __RTE_BUDDY_H__
#include "rte_types.h"
#include "rte_list.h"
#include "rte_spinlock.h"

#define RTE_MAX_ORDER 7U // Max (1<<RTE_MAX_ORDER)MB

/* 
 * The shared memory is based on Hugetlbpage, 
 * the size of hugetlbpage is 2M 
 * */
#define RTE_PAGE_SHIFT	21U 
#define RTE_PAGE_SIZE 	2097152U	// 2M

/*
 * 标记Page所处的状态
 * */
enum pageflags{
	PG_slub_frozen,
	PG_slub, // Page在Slub系统中 
	PG_head, // 
	PG_tail, //
	PG_buddy, // Page在Buddy系统中
};

/*
 * Buddy系统以页为单位管理内存块，
 * struct rte_page就是页描述符
 * */
struct rte_page{
	struct list_head lru;	
	uint64_t flags;
	rte_spinlock_t lock;
	struct{
		uint32_t inuse:16;//表示正在使用的Object的个数
		uint32_t objects:16; // 页中包含slab object的个数
	};
	union{
		uint64_t private;
		struct rte_mem_cache *slab;
		struct rte_page *first_page;
	};
	void *freelist; // 
}; 

struct free_area{
	struct list_head free_list;
	uint32_t nr_free;
};

/*
 * 要被Buddy系统管理的大块内存的描述符
 * */
struct rte_mem_zone{
	uint32_t page_num; // 内存块中页的个数
	uint32_t page_size; // 每个页的大小
	uint32_t free_zero_num;
	struct rte_page *first_page;
	uint64_t start_addr; // 内存块起始地址
	uint64_t end_addr; // 内存块结束地址
	struct free_area free_area[RTE_MAX_ORDER]; // 空闲页链表
	rte_spinlock_t lock;
};

static inline void RTE_BUDDY_BUG(char *f, int line)
{
	printf("BUDDY_BUG in %s, %d.\n", f, line);
	// rte_assert(0);
}

/*
 * 页分为两类：一类是单页（zero page）,一类是组合页（compound page）
 *
 * */
static inline void __SetPageHead(struct rte_page *page)
{
	page->flags |= PG_head;		
}

static inline void __SetPageTail(struct rte_page *page)
{
	page->flags |= PG_tail;
}

static inline void __SetPageBuddy(struct rte_page *page)
{
	page->flags |= (1UL<<PG_buddy);	
}

static inline void __SetPageSlub(struct rte_page *page)
{
	page->flags |= (1UL<<PG_slub);
}

static inline void __SetPageSlubFrozen(struct rte_page *page)
{
	page->flags |= (1UL<<PG_slub_frozen);	
}

static inline void __ClearPageBuddy(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_buddy);
}

static inline void __ClearPageHead(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_head);
}

static inline void __ClearPageTail(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_tail);
}

static inline void __ClearPageSlub(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_slub);
}

static inline void __ClearPageSlubFrozen(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_slub_frozen);
}

static inline int PageHead(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_head));	
}

static inline int PageTail(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_tail));
}

static inline int PageBuddy(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_buddy));
}

static inline int PageSlub(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_slub));
}

static inline int PageCompound(struct rte_page *page)
{
	return (page->flags & ((1UL<<PG_head)|(1UL<<PG_tail)));
}

static inline int PageSlubFrozen(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_slub_frozen));
}

static inline int compound_order(struct rte_page *page)
{
	if(!PageHead(page)) // No Head flag, it's a zero page
		return 0;
	return (unsigned long)page[1].lru.prev;
}

int rte_buddy_system_init(struct rte_mem_zone *zone, unsigned long start_addr, 
						  struct rte_page *start_page, unsigned int page_size, unsigned int page_num);
struct rte_page *rte_get_pages(unsigned int order);
void rte_free_pages(struct rte_page *page);
void *rte_page_to_virt(struct rte_page *page);
struct rte_page *rte_virt_to_head_page(void *ptr);

#endif

