/*****************************************************************
 *  ???:mem.c
 *   ????:
 *    pid ????????PID
 *     va ??????????
 *      *****************************************************************/
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/init.h>
#include <linux/sched.h>
//#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <asm/tlbflush.h>
//#include <linux/delay.h> 
//#include <linux/fs.h>
//#include <linux/uaccess.h>
//#include <linux/mm.h>

#include <linux/jit_aslr.h>
MODULE_LICENSE("Dual BSD/GPL");

struct pte_snapshot {
	unsigned long pg;
	pte_t *ptep;
	pte_t pte;
};

static struct pte_snapshot pgtable_snapshot[JIT_L2L_LIMIT];

static void print_rand(int rand[])
{
	int i;
	printk("-----rand-----\n");
	for(i=0; i<L2L_size; i++) {
		printk("[%d]=%d\n", i, rand[i]);
	}
}

static void print_pgtable_snapshot(void)
{
	int i;
	printk("-----pgtable-----\n");
	for(i=0; i<L2L_size; i++) {
		printk("%lx: %lx\n", pgtable_snapshot[i].pg, pgtable_snapshot[i].pte);
	}
}

static int get_pgtable_snapshot(void)
{
	int i;
	unsigned long pg;
	struct task_struct *tsk;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;

	if(!(tsk = pid_task(find_vpid(L2L_pid), PIDTYPE_PID))) {
		printk("Can't find the task %d .\n", L2L_pid);
		return -1;
	}
	for (i=0; i<L2L_size; i++) {
		pg = L2L_code_start + (i<<PAGE_SHIFT);

		pgd = pgd_offset(tsk->mm, pg);
		if(pgd_none(*pgd)) {
			return -1;
		}

		pud = pud_offset(pgd, pg);
		if(pud_none(*pud)) {
			return -1;
		}

		pmd = pmd_offset(pud, pg);
		if(pmd_none(*pmd)) {
			return -1;
		}

		ptep = pte_offset_kernel(pmd, pg);
		if(!ptep) {
			return -1;
		}
		pte = *ptep;
		if(!pte_present(pte)) {
			printk("Error: pte not present!\n");
			if (pte_none(pte)) {
				printk("Error: pte none!\n");
			}
			return -1;
		}

		pgtable_snapshot[i].pg = pg;
		pgtable_snapshot[i].ptep = ptep;
		pgtable_snapshot[i].pte = pte;
	}
	return 0;
}

static void randomize(int rand[], int size)
{
	int from_index;
	int to_index;
	int temp;
	void *ptr_to_index = &to_index;

	for (from_index=0; from_index<size; from_index++) {
		//get a random to_index
		get_random_bytes(ptr_to_index, sizeof(int));
		to_index = ((unsigned int)to_index) % size;
		//swap
		temp = rand[to_index];
		rand[to_index] = rand[from_index];
		rand[from_index] = temp;
	}
}

static void undo_current_page(int rand[], int size, int from_index)
{
	int to_index;
	int i;
	int temp;

	for(i=0; i<size; i++) {
		if (rand[i] == from_index) {
			to_index = i;
			break;
		}
	}
	temp = rand[to_index];
	rand[to_index] = rand[from_index];
	rand[from_index] = temp;
}

static void update_L2L_map(int rand[], int size)
{
	int real_index;
	int new_real_index;
	unsigned long new_real;
	int i;
	
	for (i=0; i<size; i++) {
		real_index = page_to_index(L2L_map[i].real);
		new_real_index = rand[real_index];
		new_real = index_to_page(new_real_index);
		L2L_map[i].real = new_real;
	}
}

static int do_jit_aslr(void)
{
	int i;
	int rand[L2L_size];
	int current_page_index;

	L2L_print_basics();
	// init the rand mapping
	for (i=0; i<L2L_size; i++) {
		rand[i] = i;
	}
	//printk("init...\n");
	//print_rand(rand);
	// aquire the lock
	down_write(&L2L_sem);
	// randomize the mapping
	randomize(rand, L2L_size);
	//printk("randomize...\n");
	//print_rand(rand);
	// undo the randomization on the current page
	current_page_index = (L2L_current_page - L2L_code_start) >> PAGE_SHIFT;
	undo_current_page(rand, L2L_size, current_page_index);
	//printk("undo current page...\n");
	print_rand(rand);
	// get the page table entry info
	if(get_pgtable_snapshot()) {
		printk("Failed to get page table snapshot\n");
		up_write(&L2L_sem);
		return -1;
	}
	L2L_print_map();
	print_pgtable_snapshot();
	// apply the randomization
	for (i=0; i<L2L_size; i++) {
		*(pgtable_snapshot[i].ptep) = pgtable_snapshot[rand[i]].pte;
	}
	__flush_tlb_all();
	// populate the L2L map
	update_L2L_map(rand, L2L_size);
	//L2L_print_map();
	//print_pgtable_snapshot();
	// release the lock
	up_write(&L2L_sem);
	if(get_pgtable_snapshot()) {
		printk("Failed to get page table snapshot\n");
		return -1;
	}
	L2L_print_map();
	print_pgtable_snapshot();
	return 0;
}

static int jit_aslr_init(void)
{
	do_jit_aslr();
	return 0;
	while(!L2L_stop) {
		//randomize every 20 seconds
		msleep_interruptible(5000); //sleep 5 seconds
		if(do_jit_aslr()) {
			break;
		}
	}
	return 0;
}

static void jit_aslr_exit(void)
{
	printk("Goodbye!\n");
	printk("\n");
}

module_init(jit_aslr_init);
module_exit(jit_aslr_exit);
