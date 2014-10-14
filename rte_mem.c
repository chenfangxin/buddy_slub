#include "rte_slub.h"

void  *rte_malloc(int size)
{
	void *ptr=NULL;
	ptr = __rte_slub_alloc(size);

	return ptr;	
}

void rte_free(void *ptr)
{
	__rte_slub_free(ptr);
}
