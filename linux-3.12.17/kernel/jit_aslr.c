/*
   Copyright (C) 2014 Ping Chen
   Copyright (C) 2014 Jun Wang

   This program is free software; you can redivaluestribute it and/or modify
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
#include <linux/delay.h>

#define SIZE_SHIFT 28
#define JMP_TYPE_SHIFT 20

#define get_inst_type(x) ((x) & 0x0000000F) //e.g. get_inst_type(flags) ?????????(init, ret, jmp, end-of-page...)
#define get_addr_type(x) ((x) & 0x000F0000) // e.g.  get_addr_type(flags) ???????????????
#define decode_size(x)  (((x) & 0xF0000000) >> SIZE_SHIFT)
#define get_cond_jmp_type(x) (((x) & 0x0FF00000) >> JMP_TYPE_SHIFT)


#define JMP_CALL_EAX              0x00010000
#define JMP_CALL_INDIR_OFF_EAX_N  0x00020000
#define JMP_CALL_INDIR_EAX        0x00040000
#define JMP_CALL_INDIR_EAX_OFF    0x00080000
#define JMP_CALL_MASK  0x0000000f
#define DYNS_INST_INIT 0x00000000
#define DYNS_INST_RET  0x00000001
#define DYNS_INST_CALL 0x00000002
#define DYNS_INST_JUMP 0x00000004
#define DYNS_INST_PAGE 0x00000008
#define DYNS_INST_MASK 0x0000000f 
#define COND_JMP_SHIFT             20
#define COND_JMP_JO                (1UL  << COND_JMP_SHIFT)    
#define COND_JMP_JNO               (2UL  << COND_JMP_SHIFT)
#define COND_JMP_JS                (3UL  << COND_JMP_SHIFT)
#define COND_JMP_JNS               (4UL  << COND_JMP_SHIFT)
#define COND_JMP_JE_JZ             (5UL  << COND_JMP_SHIFT)
#define COND_JMP_JNE_JNZ           (6UL  << COND_JMP_SHIFT)
#define COND_JMP_JB_JNAE_JC        (7UL  << COND_JMP_SHIFT)
#define COND_JMP_JNB_JAE_JNC       (8UL  << COND_JMP_SHIFT)
#define COND_JMP_JBE_JNA           (9UL  << COND_JMP_SHIFT)
#define COND_JMP_JA_JNBE           (10UL << COND_JMP_SHIFT)
#define COND_JMP_JL_JNGE           (11UL << COND_JMP_SHIFT)
#define COND_JMP_JGE_JNL           (12UL << COND_JMP_SHIFT)
#define COND_JMP_JLE_JNG           (13UL << COND_JMP_SHIFT)
#define COND_JMP_JG_JNLE           (14UL << COND_JMP_SHIFT)
#define COND_JMP_JP_JPE            (15UL << COND_JMP_SHIFT)
#define COND_JMP_JNP_JPO           (16UL << COND_JMP_SHIFT)
#define COND_JMP_JCXZ              (17UL << COND_JMP_SHIFT)
#define COND_JMP_JECXZ             (18UL << COND_JMP_SHIFT)

struct JIT_L2L_mapping L2L_map[JIT_L2L_LIMIT]; //protected by L2L_sem
unsigned long L2L_current_page; //protected by L2L_sem
unsigned long L2L_code_start;
int L2L_size;
pid_t L2L_pid;
int L2L_stop=1;
struct rw_semaphore L2L_sem;
unsigned long * syscall_p;

EXPORT_SYMBOL(L2L_map);
EXPORT_SYMBOL(L2L_current_page);
EXPORT_SYMBOL(L2L_code_start);
EXPORT_SYMBOL(L2L_size);
EXPORT_SYMBOL(L2L_pid);
EXPORT_SYMBOL(L2L_stop);
EXPORT_SYMBOL(L2L_sem);
EXPORT_SYMBOL(syscall_p);

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

    if (index >= JIT_L2L_LIMIT || index<0) {
        printk("index %d out of bound of the L2L mapping (size %d)\n", index, JIT_L2L_LIMIT);
        return page;
    }
    //we already hold the L2L_sem since the entrance of syscall hello
    real_page = L2L_map[index].real;
    return real_page;
}


static unsigned long get_original_logical_address(unsigned long address)
{
    return address;
    unsigned long page = (address) >> PAGE_SHIFT;
    unsigned long real_address;

    real_address = address;
    //we search all the pages in the L2L map
    int i = 0;
    for(i = 0;i<JIT_L2L_LIMIT; i++){
        if(L2L_map[i].real == page) {
            real_address = L2L_map[i].orig | (address& ~PAGE_MASK);
            printk("We get the real original logical address %x\n", real_address);
        }
    }

    return real_address;
}


static unsigned long get_real_logical_address(unsigned long address)
{
    //return address;
    //printk(" Unrandomized address: %x,%x,%x\n",address,L2L_code_start,L2L_size*PAGE_SIZE+L2L_code_start);
    if(address<=L2L_code_start || L2L_size == 0 || address>= L2L_size*PAGE_SIZE + L2L_code_start) {

        return address;
    }
    unsigned long page = address & PAGE_MASK;
    unsigned long real_page = get_real_page(page);
    //printk("Randomized address:%x\n", real_page | (address & ~PAGE_MASK));

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
    if(sys_mlock(L2L_code_start, (L2L_size)*(PAGE_SIZE))) {
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


SYSCALL_DEFINE3(hello, int, addr1, int, flag, int, addr2)
{
    /*if(flag == 0x8c03100) {
      return 0;
      }*/
    printk("XXXXXXXXXXXXXXXXXXX\n");
    unsigned long *sp = 0;
    unsigned long *stack;
    unsigned long *stack_bak;
    unsigned long stack_call[4];
    unsigned long eip_buffer[4];
    unsigned long customer_stack[10];
    unsigned long *pc;
    unsigned long pc_temp[100];
    unsigned long pc_final;
    unsigned long *temp_pc;
    unsigned long pc_tmp;
    unsigned long pc_real;
    unsigned long eax;
    unsigned long ebx;
    unsigned long ecxvalue_ret;
    unsigned long edxvalue_ret;
    unsigned long esivalue_ret;
    unsigned long edivalue_ret;
    unsigned long ecxvalue;
    unsigned long edxvalue;
    unsigned long esivalue;
    unsigned long edivalue;
    unsigned long esp;
    unsigned long ebpvalue;
    unsigned long ebpvalue_ret;
    unsigned long eip;
    unsigned long next_eip;
    unsigned long saved_eip;
    unsigned long eflags;
    unsigned long cf;
    unsigned long pf;
    unsigned long af;
    unsigned long zf;
    unsigned long sf;
    unsigned long tf;
    unsigned long If;
    unsigned long df;
    unsigned long of;
    unsigned long nt;
    unsigned long rf;
    unsigned long cx;
    unsigned long * customer_saved;
    unsigned long top1;
    unsigned long top2;
    unsigned long top3;
    unsigned long top4;
    unsigned long from2;
    unsigned long jmp_type;
    unsigned long jcc_type;
    int end_flag = 0; 
    int internal_flag =0;
    int pos = 0;
    int i, k, q;
    q = 0;
    /*flag types*/
    /*init 0x00000000*/
    /*ret 0x00000001*/
    /*call general format 0x00000002*/
    /*call EAX 0x00010002*/
    /*call [EAX] 0x00040002*/
    /*call call [EAX + 10]  0x00080002*/
    /*jmp general format 0x00000004*/
    /*jmp eax 0x00010004*/
    /*jmp [80b3828 + EAX * 4] 0x00020004*/
    /*last page 0x00000008*/

    if(flag == 0) {
        /*We wrap the ajacent pages of the syscall 0x8049180  0x8048180 0x8047180 0x8046180 0x804a180 0x804b180*/
        /*unsigned long cr0;
          cr0 = read_cr0();
          write_cr0(cr0 & ~X86_CR0_WP);
          if(syscall_p == 0) {
          syscall_p = 0x8049180;
          }
          unsigned long syscall_stack[20];

          unsigned long offset = 0;
          unsigned long offset_stack[2];
          unsigned long * temp = 0x804918c;
          unsigned long * target = 0;
          unsigned long * to =0;
          unsigned long temp_address = 0;

        // 0x8048180
        copy_from_user((void *)offset_stack,(void *)temp,8);
        copy_from_user((void *)syscall_stack,(void *)syscall_p,20);
        int i = 0;
        for(i = 0;i<6;i++) {
        printk("[%d]:%x\n",i,syscall_stack[i]);
        }
        offset = offset_stack[0];
        offset = offset + PAGE_SIZE;

        temp_address = &syscall_stack;
        target = temp_address + 0xc;
         *target = offset;

         temp_address = syscall_p;
         to = temp_address - PAGE_SIZE;
         copy_to_user((void *)to,(void*)syscall_stack,17);
         printk("XXXX[-1] %x,%x,%x,%x,%x\n",offset,syscall_p,to,target,syscall_stack);

        // 0x8047180
        copy_from_user((void *)offset_stack,(void *)temp,8);
        copy_from_user((void *)syscall_stack,(void *)syscall_p,20);
        offset = offset_stack[0];
        offset = offset + 2*PAGE_SIZE;

        temp_address = &syscall_stack;
        target = temp_address + 0xc;
        *///*target = offset;

        /*temp_address = syscall_p;
          to = temp_address - 2*PAGE_SIZE;
          copy_to_user((void *)to,(void*)syscall_stack,17);
          printk("XXXX[-2] %x,%x,%x,%x,%x\n",offset,syscall_p,to,target,syscall_stack);

        //copy_from_user((void *)syscall_stack,(void*)to,17);
        for(i = 0;i<6;i++) {
        printk("[%d]:%x\n",i,syscall_stack[i]);
        }


        // 0x804a180
        copy_from_user((void *)offset_stack,(void *)temp,8);
        copy_from_user((void *)syscall_stack,(void *)syscall_p,20);
        offset = offset_stack[0];
        offset = offset - PAGE_SIZE;

        temp_address = &syscall_stack;
        target = temp_address + 0xc;
         *target = offset;

         temp_address = syscall_p;
         to = temp_address + PAGE_SIZE;
         copy_to_user((void *)to,(void*)syscall_stack,17);
         printk("XXXX[1] %x,%x,%x,%x,%x\n",offset,syscall_p,to,target,syscall_stack);
        // 0x804b180 
        copy_from_user((void *)offset_stack,(void *)temp,8);
        copy_from_user((void *)syscall_stack,(void *)syscall_p,20);
        offset = offset_stack[0];
        offset = offset - 2*PAGE_SIZE;

        temp_address = &syscall_stack;
        target = temp_address + 0xc;
        *///*target = offset;

        /*temp_address = syscall_p;
          to = temp_address + 2*PAGE_SIZE;
        //copy_to_user((void *)to,(void*)syscall_stack,17);
        printk("XXXX[2] %x,%x,%x,%x,%x\n",offset,syscall_p,to,target,syscall_stack);

        write_cr0(cr0);
        */
        init_hello(0x8100000,0x8103000);
        msleep(10000);
        return 0;
    }        

    //down_read(&L2L_sem);
    sp = (unsigned long *)current->thread.sp;
    stack = sp;
    stack_bak = sp;
    printk("flag %x,%x,%x,%x\n",flag,get_inst_type(flag),addr1,addr2);

    if((get_inst_type(flag)  == 0x1) || (get_inst_type(flag)  == 0x2) ||(get_inst_type(flag) == 0x4) || (get_inst_type(flag) == 0x8)) { // ret or call	or jmp/jcc

        //search for the target address
        printk("KERNEL Stack:");       
        for (i = 0; i < 400; i++) {
            pc_tmp = 0;

            if (kstack_end(stack))
                break;
            k = (*stack);
            //if((get_inst_type(flag) == 0x1 )|| ( get_inst_type(flag) ==0x2)|| (get_inst_type(flag)==0x4) || (get_inst_type(flag) == 0x8)) {
            printk("%d %lx\n",i,*stack);
            //}
            if (k==0x15f) {
                q = (*(stack+5));
                if (q==0x15f)
                    internal_flag = 1;
            }
            if(end_flag == 0 && k == 0x15f && internal_flag == 1) {
                pos = i;
                end_flag = 1;
                int j =0;
                unsigned long * to;
                unsigned long stack_customer[100];
                unsigned long * from;
                unsigned long * temp;
                unsigned long * eip_p;
                from = *(stack+9);
                from2 = *(stack+9);
                customer_saved = from;
                copy_from_user((void *)stack_customer,(void *)from,400);
                to = stack_customer;
                if (flag != 0 && flag !=-1) {
                    temp = to;
                    printk("User level stack:\n");
                    for(j=0;j<100;j++) {
                        if(j==0) {
                            top1 = *temp;
                        }
                        if(j==1) {
                            top2 = *temp;
                        }
                        if(j==2) {
                            top3 = *temp;
                        }
                        if(j==3) {
                            top4 = *temp;
                        }
                        printk("[%d] %lx ",j,*temp);
                        if(j == 14) { //edivalue
                            edivalue = *temp;
                        }
                        if(j == 15) { //esivalue
                            esivalue = *temp;
                        }
                        if(j == 16) { //ebpvalue
                            ebpvalue = *temp;
                        }
                        if(j == 17) { //esp
                            esp = *temp  + 4;
                        }
                        if(j == 18) { //ebx
                            ebx = *temp;
                        }
                        if(j == 19) { //edxvalue
                            edxvalue = *temp;
                        }
                        if(j == 20) { //ecxvalue
                            ecxvalue = *temp;
                            cx = ecxvalue & 0xffff;
                        }
                        if(j == 21) { //eax
                            eax = *temp;
                        }
                        if(j == 22) { //eflags
                            eflags = *temp;
                            of = ((eflags) & 0x00000800)>>11; // 11 bit
                            sf = ((eflags) & 0x00000080)>>7; // 7 bit
                            zf = ((eflags) & 0x00000040)>>6; // 6 bit
                            cf = (eflags) & 0x00000001; // 0 bit
                            pf = ((eflags) & 0x00000004)>>2; // 2 bit
                        }
                        if(j == 8) {
                            saved_eip = *temp;
                        }

                        if(get_inst_type(flag)  == 0x1) { // ret
                            unsigned long temp1[4];
                            copy_from_user((void *)temp1,(void *)esp,4);
                            eip =  temp1[0];                                                                       
                            eip = get_real_logical_address(eip);
                            //printk("UUU %x,%x,%x,%x\n",temp1[0],temp1[1],temp1[2],temp1[3]);
                            //eip = addr2;
                            //eip = get_real_logical_address(eip);
                        }
                        if(get_inst_type(flag)  == 0x2) { //call
                            long x = (flag>>28)&0x0000000f;
                            next_eip = saved_eip + 6 + x;
                            next_eip = get_original_logical_address(next_eip);
                            unsigned long jmp_type = get_addr_type(flag);
                            if(jmp_type == JMP_CALL_EAX) { // call eax
                                eip = eax;
                            }    
                            else if(jmp_type == JMP_CALL_INDIR_OFF_EAX_N) { //call [offset+eax*4]
                                unsigned long * addr_p;
                                unsigned long temp1[4];
                                addr_p = addr2 + eax *4;
                                copy_from_user((void *)temp1,(void *)addr_p,4);
                                eip = temp1[0];

                            }
                            else if(jmp_type == JMP_CALL_INDIR_EAX) { //call [eax]
                                unsigned long * addr_p;
                                unsigned long temp1[4];
                                addr_p = eax;
                                copy_from_user((void *)temp1,(void *)addr_p,4);
                                eip = temp1[0];

                            }
                            else if(jmp_type == JMP_CALL_INDIR_EAX_OFF) { // call [offset+eax]
                                unsigned long * addr_p;
                                unsigned long temp1[4];
                                addr_p = addr2 + eax;
                                copy_from_user((void *)temp1,(void *)addr_p,4);
                                eip = temp1[0];
                            }
                            else {
                                eip = addr2; 
                            }
                        }

                        if(get_inst_type(flag)  == 0x4) { //jmp/jcc
                            jmp_type = get_addr_type(flag);
                            jcc_type = get_cond_jmp_type(flag);
                            if(jcc_type == 0)  { //jmp 
                                if(jmp_type == JMP_CALL_EAX) { // call eax
                                    eip = eax;
                                }    
                                else if(jmp_type == JMP_CALL_INDIR_OFF_EAX_N) { //call [offset+eax*4]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = addr2 + eax *4;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];

                                }
                                else if(jmp_type == JMP_CALL_INDIR_EAX) { //call [eax]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = eax;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];

                                }
                                else if(jmp_type == JMP_CALL_INDIR_EAX_OFF) { // call [offset+eax]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = addr2 + eax;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];
                                }
                                else {
                                    eip = addr2; 
                                }
                            }
                            else { //jcc
                                long x = (flag>>28)&0x0000000f;
                                next_eip = saved_eip + 6 + x;
                                next_eip = get_original_logical_address(next_eip);
                                if(jmp_type == JMP_CALL_EAX) { // call eax
                                    eip = eax;
                                }    
                                else if(jmp_type == JMP_CALL_INDIR_OFF_EAX_N) { //call [offset+eax*4]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = addr2 + eax *4;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];

                                }
                                else if(jmp_type == JMP_CALL_INDIR_EAX) { //call [eax]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = eax;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];

                                }
                                else if(jmp_type == JMP_CALL_INDIR_EAX_OFF) { // call [offset+eax]
                                    unsigned long * addr_p;
                                    unsigned long temp1[4];
                                    addr_p = addr2 + eax;
                                    copy_from_user((void *)temp1,(void *)addr_p,4);
                                    eip = temp1[0];
                                }
                                else {
                                    eip = addr2; 
                                }

                                if(jcc_type == 0x1) {
                                    if(of == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x2) {
                                    if(of == 0x0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x3) {
                                    if(sf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x4) {
                                    if(sf == 0x0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x5) {
                                    if(zf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x6) {
                                    if(zf == 0x0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x7) {
                                    if(cf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x8) {
                                    if(cf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x9) {
                                    if(cf == 0x1 || zf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xa) {
                                    if(cf == 0 && zf == 0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xb) {
                                    if(sf != of) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xc) {
                                    if(sf == of) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xd) {
                                    if(sf != of || zf == 1 ) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xe) {
                                    if(sf == of || zf == 0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0xf) {
                                    if(pf == 0x1) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x10) {
                                    if(pf == 0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x11) {
                                    if(cx == 0x0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 
                                if(jcc_type == 0x12) {
                                    if(ecxvalue == 0x0) {
                                        eip = eip;
                                    }
                                    else {
                                        eip = next_eip;
                                    }
                                } 

                            }
                        }
                        if(get_inst_type(flag)  == 0x8) { // page
                            eip = addr2;
                        }

                        temp++;
                    }
                }
            }
            stack ++;
        }
    }
    printk("\n");
    printk("\n");
    printk("\n");

    eip = get_real_logical_address(eip);
    /*if(next_eip !=0) {
      next_eip = get_real_logical_address(next_eip);
      } */

    unsigned long cr0;
    cr0 = read_cr0();
    write_cr0(cr0 & ~X86_CR0_WP);

    if(flag != 0 && flag!= -1 && (get_inst_type(flag)  == 0x1|| get_inst_type(flag) == 0x2 ||  get_inst_type(flag) == 0x4 || get_inst_type(flag)== 0x8) && end_flag >= 1) {
        printk("EAX %x\n",eax);
        printk("EBX %x\n",ebx);
        printk("ECX %x\n",ecxvalue);
        printk("EDX %x\n",edxvalue);
        printk("EBP %x\n",ebpvalue);
        printk("ESI %x\n",esivalue);
        printk("EDI %x\n",edivalue);
        printk("ESP %x\n",esp);
        printk("EIP %x\n",eip);
        printk("EFLAGS %x\n",eflags);
        printk("EIP_next %x\n",next_eip);
        printk("addr1 %x\n",addr1);
        printk("eflags %x\n",flag);
        printk("addr2 %x\n",addr2);
        printk("stack_bak %x\n",stack_bak);
        /**popa*/
        //if(get_inst_type(flag) ==0x2 ) 
        //  stack_bak[pos+9] = esp-4;

        //if(flag == 1/*&&(eip==0x8048977||eip == 0x804897c||eax==0x804cb3c||eip == 0x8048fbd )*/)
        //  stack_bak[pos+9] = esp-32;


        //stack_bak[pos+8] = eflags;
        //stack_bak[pos+6] = eip;//
        //stack_bak[pos+5] = eax;
        //stack_bak[pos] = eax;
        /*
           stack_bak[pos-1] = ebpvalue;
           stack_bak[pos-2] = edivalue;
           stack_bak[pos-3] = esivalue;
           stack_bak[pos-4] = edxvalue;
           stack_bak[pos-5] = ecxvalue;
           stack_bak[pos-6] = ebx;   */              
        sp = (unsigned long *)current->thread.sp;
        if(get_inst_type(flag) == 1) {
            copy_from_user((void*)customer_stack,(void *)(esp-32),40);
            printk("Original Stack: %x, %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%d,%d\n",esp+4-from2,from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9],zf,end_flag);

            customer_stack[4] = ebx;//ebpvalue;
            customer_stack[5] = esivalue;//ebx;
            customer_stack[6] = edivalue;//esivalue;
            customer_stack[7] = ebpvalue;//edivalue;
            printk("Randomized Stack: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n",from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9]);
            copy_to_user((void *)(esp-32),(void*)customer_stack,40);
        }
        /*push next_eip ->esp-4*/
        if(get_inst_type(flag) ==0x2 ) {
            copy_from_user((void*)customer_stack,(void *)(esp-40),40);
            printk("Original Stack: %x, %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%d,%d\n",esp+4-from2,from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9],zf,end_flag);

            customer_stack[4] = ebx;//ebpvalue;
            customer_stack[5] = esivalue;//ebx;
            customer_stack[6] = edivalue;//esivalue;
            customer_stack[7] = ebpvalue;//edivalue;
            customer_stack[8] = eip;//edivalue;
            customer_stack[9] = next_eip;//edivalue;
            printk("Randomized Stack: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n",from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9]);
            copy_to_user((void *)(esp-40),(void*)customer_stack,40);
            /* 	unsigned long * kkk =esp-4 ;
            // printk("HHHHHHHHH%x\n",kkk);
            copy_from_user((void*)stack_call,(void *)kkk,4);
             *stack_call = next_eip;
             if(stack_call == NULL)
             printk("HHHHHHHHH%x\n",esp-4);
             copy_to_user((void *)kkk,(void*)stack_call,4);*/
        }
        if(get_inst_type(flag) == 0x4 || get_inst_type(flag)== 0x8) {
            copy_from_user((void*)customer_stack,(void *)(esp-36),40);
            printk("Original Stack: %x,%x, %x, %x, %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%d,%d\n",sf,eip,jcc_type,esp+4-from2,from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9],zf,end_flag);

            customer_stack[4] = ebx;//ebpvalue;
            customer_stack[5] = esivalue;//ebx;
            customer_stack[6] = edivalue;//esivalue;
            customer_stack[7] = ebpvalue;//edivalue;
            customer_stack[8] = eip;//edivalue;
            //customer_stack[9] = next_eip;//edivalue;
            printk("Randomized Stack: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n",from2,esp+4,customer_stack[0],customer_stack[1],customer_stack[2],customer_stack[3],customer_stack[4],customer_stack[5],customer_stack[6],customer_stack[7],customer_stack[8],customer_stack[9]);
            copy_to_user((void *)(esp-36),(void*)customer_stack,40);
            /* 	unsigned long * kkk =esp-4 ;
            // printk("HHHHHHHHH%x\n",kkk);
            copy_from_user((void*)stack_call,(void *)kkk,4);
             *stack_call = next_eip;
             if(stack_call == NULL)
             printk("HHHHHHHHH%x\n",esp-4);
             copy_to_user((void *)kkk,(void*)stack_call,4);*/
        }
    }
    write_cr0(cr0);
    // printk("pc_final %x\n",pc_final);
    //pc_real = get_real_logical_address(pc_final);
    //save current_page (after returning of this syscall)
    L2L_current_page = eip & PAGE_MASK;

    //release the lock
    //up_read(&L2L_sem);
    /*__asm__("push %eax"); 
      __asm__("movl %1, %%eax" :"=r(ebpvalue_ret)" "r"(eflags)); 
      __asm__("sahf"); 
      __asm__("pop %eax"); 
      */
    return 0;
}
