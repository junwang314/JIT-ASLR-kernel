#ifndef _LINUX_JIT_ASLR_H
#define _LINUX_JIT_ASLR_H

#include <linux/types.h>

struct JIT_L2L_mapping {
	unsigned long orig;
	unsigned long real;
};

#define JIT_L2L_LIMIT	1024

extern struct JIT_L2L_mapping L2L_map[JIT_L2L_LIMIT];
extern unsigned long L2L_current_page;
extern int L2L_size;
extern unsigned long L2L_code_start;
extern pid_t L2L_pid;
extern int L2L_stop;
extern struct rw_semaphore L2L_sem;

extern void L2L_print_basics(void);
extern void L2L_print_map(void);

static inline int page_to_index(unsigned long page)
{
	int index;
	index = (page - L2L_code_start) >> PAGE_SHIFT;
	return index;
}

static inline int index_to_page(int index)
{
	unsigned long page;
	page = L2L_code_start + (index << PAGE_SHIFT);
	return page;
}

/*
walk_page_range
PMD_SHIFT
PUD_SHIFT
pte_page
pte_unmap
pte_offset_kernel
pte_offset_map
pid_task
task_struct
find_vpid
PIDTYPE_PID
follow_page
flush_tlb_range
flush_cache_range
__flush_tlb_all
msleep_interruptable
bad_pud
msleep
pid_t
*/
#endif //_LINUX_JIT_ASLR_H
