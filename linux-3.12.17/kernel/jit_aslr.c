/*
   Copyright (C) 2014 Ping Chen
   Copyright (C) 2014 Jun Wang

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <asm/uaccess.h>
#include <linux/jit_aslr.h>

struct JIT_L2L_mapping L2L_map[JIT_L2L_LIMIT]; //protected by L2L_sem
unsigned long L2L_current_page; //protected by L2L_sem
unsigned long L2L_code_start;
int L2L_size;
pid_t L2L_pid;
int L2L_stop=1;
struct rw_semaphore L2L_sem;

EXPORT_SYMBOL(L2L_map);
EXPORT_SYMBOL(L2L_current_page);
EXPORT_SYMBOL(L2L_code_start);
EXPORT_SYMBOL(L2L_size);
EXPORT_SYMBOL(L2L_pid);
EXPORT_SYMBOL(L2L_stop);
EXPORT_SYMBOL(L2L_sem);

void L2L_print_basics(void)
{
	printk("code_start=%lx, nr_pages=%d, current_page=%lx\n", L2L_code_start, L2L_size, L2L_current_page);
}
EXPORT_SYMBOL(L2L_print_basics);

void L2L_print_map(void)
{
	int i;
	printk("-----L2L_map-----\n");
	printk("[orig]=real\n");
	for(i=0; i<L2L_size; i++) {
		printk("[%lx]=%lx\n", L2L_map[i].orig, L2L_map[i].real);
	}
}
EXPORT_SYMBOL(L2L_print_map);

static unsigned long get_real_page(unsigned long page)
{
	int index = (page - L2L_code_start) >> PAGE_SHIFT;
	unsigned long real_page;

	if (index >= JIT_L2L_LIMIT) {
		printk("index %d out of bound of the L2L mapping (size %d)\n", index, JIT_L2L_LIMIT);
		return 0;
	}
	//we already hold the L2L_sem since the entrance of syscall hello
	real_page = L2L_map[index].real;
	return real_page;
}

static unsigned long get_real_logical_address(unsigned long address)
{
	unsigned long page = address & PAGE_MASK;
	unsigned long real_page = get_real_page(page);

	return (real_page | (address & ~PAGE_MASK));
}

static int init_hello(unsigned long start, unsigned long end)
{
	int i;
	unsigned long page;

	//init pid, code_start, and semaphore
	L2L_pid = task_pid_nr(current);
	L2L_code_start = start;
	L2L_size = (end - start) >> PAGE_SHIFT;
	init_rwsem(&L2L_sem);

	//pin all pages into RAM
	if(sys_mlock(L2L_code_start, L2L_size*PAGE_SIZE)) {
		printk("Error: failed to do sys_mlock\n");
		return -1;
	}

	//init the L2L_map
	down_write(&L2L_sem);
	for (i=0; i<L2L_size; i++) {
		page = L2L_code_start + (i<<PAGE_SHIFT);
		L2L_map[i].orig = page;
		L2L_map[i].real = page;
	}
	L2L_current_page = L2L_code_start;
	up_write(&L2L_sem);

	//auto load kernel module
	//TODO
	printk("sys_hello init complete...\n");
	L2L_print_basics();
	L2L_print_map();
	printk("--------------------------\n");
	L2L_stop = 0;
	return 0;
}

static void exit_hello(void)
{
	L2L_stop = 1;
}

SYSCALL_DEFINE3(hello, int, addr1, int, addr2, int, flag)
{
	unsigned long *sp ;
	unsigned long *stack;
	unsigned long *pc;
	unsigned long pc_temp[100];
	unsigned long pc_final;
	unsigned long *temp_pc;
	unsigned long pc_tmp;
	unsigned long pc_real;
	int i, k;

	sp = (unsigned long *)current->thread.sp;
	stack = sp;

	if (flag == 3) {
		init_hello(addr1, addr2);
		return 0;
	}
	if (flag == 4) {
		exit_hello();
		return 0;
	}
	//hold the lock
	down_read(&L2L_sem);

	//search for the target address
	printk("KERNEL Stack:");       
	for (i = 0; i < 120; i++) {
		pc_tmp = 0;

		if (kstack_end(stack))
			break;
		k = (*stack)&0xbf000000;
		if(flag ==1) {
			printk("PC:%x,%x\n",addr1,addr2);
		}
		if(flag ==1) {
			printk("%d %lx\n",i,*stack);
		}
		if(i==103 && (k==0xbf000000)) {
			int j =0;
			unsigned long * to;
			unsigned long stack_customer[100];
			unsigned long * from;
			unsigned long * temp;
			from = *stack;
			copy_from_user((void *)stack_customer,(void *)from,100);
			to = stack_customer;
			if (flag == 1) {
				temp = to;
				if(*(to+8)==0) {
					printk("Kernel stack:\n");
					for(j=0;j<20;j++) {
						printk("%d %lx ",j,*temp);
					}
					printk("\n");
				}

				if(*(to+8)!=0) {
					pc = *(to+8)+8;
					printk("PC%lx\n",*(to+8)+8);
					pc_tmp = *(to+8)+8;
					copy_from_user((void *) pc_temp,(void *)pc,10);
					printk("PC_TMP%lx\n",pc_tmp);
					printk("PC_TEMP%lx\n",pc_temp);
					char * tempc = pc_temp;
					tempc = tempc+1;
					temp_pc = (int *)tempc;
					pc_final = *temp_pc;
					printk("PC_TMP%lx\n",pc_final);
					pc_final = pc_final + pc_tmp +5;
					printk("PC_FINAL%lx\n",pc_final);
				}   
			}
			break;
		}
		stack++;
	}
	printk("\n");
	printk("\n");
	printk("\n");

	pc_real = get_real_logical_address(pc_final);
	//save current_page (after returning of this syscall)
	L2L_current_page = pc_real & PAGE_MASK;

	//release the lock
	up_read(&L2L_sem);
	return 0;
}
