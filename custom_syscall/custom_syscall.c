#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>


asmlinkage long sys_struhar_hello(void) {
    printk("Vaclav!!!!\n");
    return 0;
}
