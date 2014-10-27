#ifndef __RTE_SPINLOCK_H__
#define __RTE_SPINLOCK_H__
typedef struct{
	volatile int value;
}rte_spinlock_t;

/* Initialize the spinlock to an unlocked state */
static inline void rte_spinlock_init(rte_spinlock_t *sl)
{
	sl->value = 0;
}

static inline void __rte_spinlock_lock__(rte_spinlock_t *sl)
{
	int lock_val = 1; // From DPDK
	__asm__ __volatile__(
			"1:\n"
			"xchg %[value], %[lv]\n"
			"test %[lv], %[lv]\n"
			"jz 3f\n"
			"2:\n"
			"pause\n"
			"cmpl $0, %[value]\n"
			"jnz 2b\n"
			"jmp 1b\n"
			"3:\n"
			: [value] "=m" (sl->value), [lv] "=q" (lock_val)
			: "[lv]" (lock_val)
			: "memory");
}

static inline void rte_spinlock_unlock(rte_spinlock_t *sl)
{
	int unlock_val = 0;
	__asm__ __volatile__ (
			"xchg %[value], %[ulv]\n"
			: [value] "=m" (sl->value), [ulv] "=q" (unlock_val)
			: "[ulv]" (unlock_val)
			: "memory");
}

static inline int rte_spinlock_locked(rte_spinlock_t *sl)
{
	return sl->value;
}

/*
 * Try to take the lock
 * return:
 * 	1: success; 0 otherwise.
 * */
static inline int __rte_spinlock_trylock__(rte_spinlock_t *sl)
{
	int lockval = 1;
	__asm__ __volatile__(
			"xchg %[value], %[lockval]"
			: [value] "=m" (sl->value), [lockval] "=q" (lockval)
			: "[lockval]" (lockval)
			: "memory");
	return (lockval == 0);
}

#define rte_spinlock_lock(lock) __rte_spinlock_lock__(lock)
#define rte_spinlock_trylock(lock) __rte_spinlock_trylock__(lock)

#endif
