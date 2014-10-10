#include <stdio.h>
#include "rte_buddy.h"
#include "rte_slub.h"

static struct rte_mem_cache *global_mem_caches;

static inline int rte_get_self_id(void)
{
	return 0;
}

static inline struct mem_cache_cpu *get_cpu_slab(struct rte_mem_cache *s, int id)
{
	return &(s->cpu_slab[id]);
}

#define for_each_object(__p, __s, __addr, __objects) \
		for(__p=(__addr);__p<(__addr)+(__objects)*(__s)->size;\
						__p += (__s)->size)

static inline void set_freepointer(struct rte_mem_cache *s, void *object, void *fp)
{
	*(void **)(object+s->offset) = fp;	
}

static inline void *get_freepointer(struct rte_mem_cache *s, void *object)
{
	return *(void **)(object + s->offset);
}

static inline unsigned long rte_oo_make(int order, unsigned long size)
{
	unsigned long x = {(order<<RTE_OO_SHIFT) + (RTE_PAGE_SIZE<<order)/size};
	return x;
}

static inline int rte_oo_order(unsigned long x)
{
	return x>>RTE_OO_SHIFT;
}

static inline int rte_oo_objects(unsigned long x)
{
	return x & RTE_OO_MASK;
}

static struct rte_page *allocate_slab(struct rte_mem_cache *s)
{
	struct rte_page *page;			
	int order = rte_oo_order(s->oo); 
	
	page = rte_get_pages(order);
	if(NULL==page){
		return NULL;
	}
	page->objects = rte_oo_objects(s->oo);

	return page;
}

static struct rte_page *new_slab(struct rte_mem_cache *s)
{
	struct rte_page *page;
	void *start;
	void *last;
	void *p;

	page = allocate_slab(s);
	if(NULL==page){
		goto out;
	}
	
	page->slab = s;
	__SetPageSlub(page);
	start = rte_page_to_virt(page);
	last = start;
	for_each_object(p, s, start, page->objects){
		set_freepointer(s, last, p);
		last = p;
	}
	set_freepointer(s, last, NULL);
	
	page->freelist = start;
	page->inuse = 0;
out:
	return page;
}

static struct rte_mem_cache *get_slab(uint32_t size)
{
	struct rte_mem_cache *s;	
	int index=0;
	if(size<=RTE_SLAB_BASE_SIZE){
		index = 0;
	}else{
		index = rte_fls(size-1) - rte_fls(RTE_SLAB_BASE_SIZE) + 1;
	}

	if(index>=RTE_SHM_CACHE_NUM){
		return NULL;
	}

	s = global_mem_caches + index;	
	return s;
}

static void slab_lock(struct rte_page *page)
{
	rte_spinlock_lock(&page->lock);
}

static void slab_unlock(struct rte_page *page)
{
	rte_spinlock_unlock(&page->lock);
}

static int slab_trylock(struct rte_page *page)
{
	return rte_spinlock_trylock(&page->lock);
}

static inline int lock_and_freeze_slab(struct mem_cache_node *n, struct rte_page *page)
{
	if(slab_trylock(page)){
		list_del(&page->lru);
		n->nr_partial--;
		__SetPageSlubFrozen(page);
		return 1;
	}
	return 0;
}

static inline struct mem_cache_node *get_node(struct rte_mem_cache *s)
{
	return &(s->local_node);
}

static void free_slab(struct rte_mem_cache *s, struct rte_page *page)
{
	__ClearPageSlub(page);	
	rte_free_pages(page);
}


static void discard_slab(struct rte_mem_cache *s, struct rte_page *page)
{
	free_slab(s, page);
}

static void add_partial(struct mem_cache_node *n, struct rte_page *page, int tail)
{
	rte_spinlock_lock(&n->list_lock);			
	n->nr_partial++;
	if(tail){
		list_add_tail(&page->lru, &n->partial);
	}else{
		list_add(&page->lru, &n->partial);
	}
	rte_spinlock_unlock(&n->list_lock);
	return;
}

static struct rte_page *get_partial(struct rte_mem_cache *s)
{
	struct rte_page *page, *page2;	
	struct mem_cache_node *n = &s->local_node;	

	if(!n||!n->nr_partial){
		return NULL;
	}
	rte_spinlock_lock(&n->list_lock);
	list_for_each_entry_safe(page, page2, &n->partial, lru){
		if(lock_and_freeze_slab(n, page))
			goto out;
	}
	page = NULL;
out:
	rte_spinlock_unlock(&n->list_lock);
	return page;
}

static void unfreeze_slab(struct rte_mem_cache *s, struct rte_page *page, int tail)
{
	struct mem_cache_node *n = get_node(s);

	__ClearPageSlubFrozen(page);
	if(page->inuse){
		if(page->freelist){
			add_partial(n, page, tail);
		}
		slab_unlock(page);
	}else{
		if(n->nr_partial < s->min_partial){
			add_partial(n, page, 1);
			slab_unlock(page);
		}else{
			slab_unlock(page);
			discard_slab(s, page);
		}
	}
}

static void deactive_slab(struct rte_mem_cache *s, struct mem_cache_cpu *c)
{
	struct rte_page *page = c->page;	
	int tail = -1;	

	while(unlikely(c->freelist)){
		void **object;	
		tail = 0;

		object = c->freelist;
		c->freelist = get_freepointer(s, c->freelist);
		set_freepointer(s, object, page->freelist);
		page->freelist = object;
		page->inuse--;
	}
	c->page = NULL;
	
	unfreeze_slab(s, page, tail);
}

static void flush_slab(struct rte_mem_cache *s, struct mem_cache_cpu *c)
{
	slab_lock(c->page);
	deactive_slab(s, c);
}

static void *__slab_alloc(struct rte_mem_cache *s, struct mem_cache_cpu *c)
{
	void **object;
	struct rte_page *new;	

	if(!c->page){//还没有给Local slab分配page
		goto new_slab;
	}

	slab_lock(c->page);
load_freelist: 
	object = c->page->freelist;//其他Core可能释放了本Page的Obj
	if(unlikely(!object)){
		goto another_slab;
	}
	c->freelist = get_freepointer(s, object);
	c->page->inuse = c->page->objects;
	c->page->freelist = NULL;

	slab_unlock(c->page);
	return object;

another_slab:
	deactive_slab(s, c);
new_slab:
	new = get_partial(s); // 从半空闲状态的page中获取一个
	if(new){
		c->page = new;
		goto load_freelist;
	}

	new = new_slab(s);
	if(new){
		c = get_cpu_slab(s, rte_get_self_id());
		if(c->page){ // 
			flush_slab(s, c);
		}
		slab_lock(new);
		__SetPageSlubFrozen(new);
		c->page = new;
		goto load_freelist;
	}
	return NULL;
}

static void *slab_alloc(struct rte_mem_cache *s)
{
	void **object;		
	struct mem_cache_cpu *c;

	c = get_cpu_slab(s, rte_get_self_id());
	object = c->freelist;
	if(unlikely(NULL==object)){//当前Local slab中没有空闲Obj
		object = __slab_alloc(s, c);
	}else{
		c->freelist = get_freepointer(s, object); 
	}

	return object;
}

void *__rte_slub_alloc(uint32_t size)
{
	struct rte_mem_cache *s;	
	void *ret;

	s = get_slab(size);
	if(unlikely(NULL==s)){
		return NULL;
	}
	ret = slab_alloc(s);
	if(NULL!=ret){
		s->alloc_cnt++;	
	}
	return ret;
}

static void remove_partial(struct rte_mem_cache *s, struct rte_page *page)
{
	struct mem_cache_node *n = get_node(s);
	rte_spinlock_lock(&n->list_lock);
	list_del(&page->lru);
	n->nr_partial--;
	rte_spinlock_unlock(&n->list_lock);
	return;
}

static void init_mem_cache_node(struct mem_cache_node *n)
{
	rte_spinlock_init(&n->list_lock);	
	n->nr_partial = 0;
	INIT_LIST_HEAD(&n->partial);
}

static void init_mem_cache_cpu(struct mem_cache_cpu *c)
{
	c->freelist = NULL;
	c->page = NULL;
}

#define MIN_PARTIAL 5
#define MAX_PARTIAL 10
static void set_min_partial(struct rte_mem_cache *s, unsigned long min)
{
	if(min<MIN_PARTIAL)
		min = MIN_PARTIAL;
	else if(min>MAX_PARTIAL)
		min = MAX_PARTIAL;
	s->min_partial = min;
}

#define RTE_SLUB_OFFSET 32
static int init_mem_cache(struct rte_mem_cache *s, int size)
{
	int i;
	s->size = size;
	s->offset = RTE_SLUB_OFFSET;
	s->oo = rte_oo_make(0, size);

	set_min_partial(s, 5);
	init_mem_cache_node(&s->local_node);

	for(i=0;i<RTE_MAX_CPU_NUM;i++){
		init_mem_cache_cpu(s->cpu_slab+i);
	}

	return 0;
}

int rte_slub_system_init(struct rte_mem_cache *array, int cache_num)
{
	int i;
	struct rte_mem_cache *s;
	int size;
	
	global_mem_caches = array;
	for(i=0;i<cache_num;i++){
		s = array + i;
		size = RTE_SLAB_BASE_SIZE * (1<<i);
		init_mem_cache(s, size);
	}
	return 0;
}

static void __slab_free(struct rte_mem_cache *s, struct rte_page *page, void *p)
{
	void *prior;
	void **object = (void *)p;

	slab_lock(page);
	prior = page->freelist;
	set_freepointer(s, object, prior);
	page->freelist = object;
	page->inuse--;
	if(unlikely(PageSlubFrozen(page))){
		goto out_unlock;
	}
	
	if(unlikely(!page->inuse)){
		goto slab_empty;
	}

	if(unlikely(!prior)){
		add_partial(get_node(s), page, 1);
	}

out_unlock:
	slab_unlock(page);
	return;

slab_empty:
	if(prior){
		remove_partial(s, page);
	}
	slab_unlock(page);

	discard_slab(s, page);

	return;
}

static void slab_free(struct rte_mem_cache *s, struct rte_page *page, void *p)
{
	void **object = (void *)p;
	struct mem_cache_cpu *c;

	c = get_cpu_slab(s, rte_get_self_id());
	if(likely(page==c->page)){
		set_freepointer(s, object, c->freelist);
		c->freelist = object;
	}else{
		__slab_free(s, page, p);
	}
	s->free_cnt++;
	return ;
}

void  __rte_slub_free(void *ptr)
{
	struct rte_page *page;	
	void *object = (void *)ptr;

	if(unlikely(NULL==ptr)){
		return;
	}

	page = rte_virt_to_head_page(ptr);
	if(unlikely(!PageSlub(page))){
		RTE_SLUB_BUG(__FILE__, __LINE__);		
		return;
	}

	slab_free(page->slab, page, object);
	return ;
}
