#ifndef __RTE_BUDDY_H__
#define __RTE_BUDDY_H__
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
	PG_slab,
	PG_head,
	PG_tail,
	PG_buddy, // Page是否在Buddy系统中
};

struct rte_page{
	struct list_head lru;	
	unsigned long flags;
	rte_spinlock_t lock;
	struct{
		unsigned int inuse:16;//表示正在使用的Object的个数
		unsigned int objects:16; // Page中包含slab object的个数
	};
	union{
		unsigned long private;
		struct rte_mem_cache *slab;
		struct rte_page *first_page;
	};
	void *freelist; // 
}; 
struct free_area{
	struct list_head free_list;
	unsigned int nr_free;
};

struct rte_mem_zone{
	unsigned int page_num;
	unsigned int page_size;
	unsigned int free_zero_num;
	struct rte_page *first_page;
	unsigned long start_addr;
	unsigned long end_addr;
	struct free_area free_area[RTE_MAX_ORDER];
	rte_spinlock_t lock;
};

static inline void RTE_BUDDY_BUG(char *f, int line)
{
	printf("BUDDY_BUG in %s, %d.\n", f, line);
	rte_assert(0);
}

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

static inline void __SetPageSlab(struct rte_page *page)
{
	page->flags |= (1UL<<PG_slab);
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

static inline void __ClearPageSlab(struct rte_page *page)
{
	page->flags &= ~(1UL<<PG_slab);
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

static inline int PageSlab(struct rte_page *page)
{
	return (page->flags & (1UL<<PG_slab));
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

int rte_buddy_system_init(struct rte_mem_zone *zone, int node_id, unsigned long start_addr, 
						  struct rte_page *start_page, unsigned long page_size, unsigned long page_num);
struct rte_page *rte_get_pages(unsigned int order, int node_id);
void rte_free_pages(struct rte_page *page, int node_id);
void *rte_page_to_virt(struct rte_page *page, int node_id);

#endif
