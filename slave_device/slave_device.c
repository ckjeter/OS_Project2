#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/net.h>
#include <net/sock.h>
#include <asm/processor.h>
#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/gfp.h> //#define free_page(addr) free_pages((addr), 0) #define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)
#include <asm/page.h>


#ifndef VM_RESERVED
#define VM_RESERVED   (VM_DONTEXPAND | VM_DONTDUMP)
#endif
#define DEV_NAME "slave_device"
#define PRINTFUNC() printk(KERN_ALERT DEV_NAME ": %s called.\n", __func__)
#define slave_IOCTL_CREATESOCK 0x12345677
#define slave_IOCTL_MMAP 0x12345678
#define slave_IOCTL_EXIT 0x12345679


#define BUF_SIZE 512




struct dentry  *file1;//debug file

typedef struct socket * ksocket_t;

//functions about kscoket are exported,and thus we use extern here
extern ksocket_t ksocket(int domain, int type, int protocol);
extern int kconnect(ksocket_t socket, struct sockaddr *address, int address_len);
extern ssize_t krecv(ksocket_t socket, void *buffer, size_t length, int flags);
extern int kclose(ksocket_t socket);
extern unsigned int inet_addr(char* ip);
extern char *inet_ntoa(struct in_addr *in); //DO NOT forget to kfree the return pointer

static int __init slave_init(void);
static void __exit slave_exit(void);

int slave_close(struct inode *inode, struct file *filp);
int slave_open(struct inode *inode, struct file *filp);
static long slave_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param);
ssize_t receive_msg(struct file *filp, char *buf, size_t count, loff_t *offp );

static mm_segment_t old_fs;
static ksocket_t sockfd_cli;//socket to the master server
static struct sockaddr_in addr_srv; //address of the master server

struct mmap_info {
    char* data;		/* the data */
    int reference;	/* how many times it is mmapped */
};

int mmap_fault(struct vm_fault *vmf)
{
	struct page* page;
	struct mmap_info* info;
	PRINTFUNC();
	info = (struct mmap_info*)(vmf->vma->vm_private_data);
	page = virt_to_page(info->data);
	get_page(page);
	vmf->page = page;
    return 0;
}
void mmap_dummy_open(struct vm_area_struct *vma)  
{
	return;
}

void mmap_dummy_close(struct vm_area_struct *vma)  
{
	return;
}
// vm operations struct
static const struct vm_operations_struct custom_vm_ops = {
	.open = mmap_dummy_open,
	.close = mmap_dummy_close,
	.fault = mmap_fault
};
static int custom_mmap(struct file *filp, struct vm_area_struct *vma)
{
	PRINTFUNC();
	if (remap_pfn_range(
			vma,
			vma->vm_start,
			(virt_to_phys(filp->private_data) >> PAGE_SHIFT) + vma->vm_pgoff,
			vma->vm_end - vma->vm_start,
			vma->vm_page_prot) < 0) {
		printk(KERN_ERR "custom_mmap remap_page_range failed!\n");
		return -1;
	}
	vma->vm_flags |= VM_RESERVED;
	vma->vm_ops = &custom_vm_ops;
	vma->vm_private_data = filp->private_data;
	mmap_dummy_open(vma);
    return 0;
}
//file operations
static struct file_operations slave_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = slave_ioctl,
	.open = slave_open,
	.read = receive_msg,
	.release = slave_close,
	.mmap = custom_mmap
};

//device info
static struct miscdevice slave_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "slave_device",
	.fops = &slave_fops
};

static int __init slave_init(void)
{
	int ret;
	file1 = debugfs_create_file("slave_debug", 0644, NULL, NULL, &slave_fops);

	//register the device
	if( (ret = misc_register(&slave_dev)) < 0){
		printk(KERN_ERR "misc_register failed!\n");
		return ret;
	}

	printk(KERN_INFO "slave has been registered!\n");

	return 0;
}

static void __exit slave_exit(void)
{
	misc_deregister(&slave_dev);
	printk(KERN_INFO "slave exited!\n");
	debugfs_remove(file1);
}


int slave_close(struct inode *inode, struct file *filp)
{
	// TODO: kfree or vfree
	free_pages(filp->private_data, 7);
	filp->private_data = NULL;
	return 0;
}

// TODO: kmalloc(k=ernel memory allocation) or vmalloc(virtual memory allocation)
// Reference1: https://www.linuxjournal.com/article/6930
// Reference2: http://brainychen72.blogspot.com/2013/08/linux-struct-file-privatedata.html
int slave_open(struct inode *inode, struct file *filp)
{
	// TODO here	
	alloc_pages(filp->private_data, 7);
	return 0;
}
static long slave_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
	long ret = -EINVAL;

	int addr_len ;
	unsigned int i;
	size_t len, data_size = 0;
	char *tmp, ip[20], buf[BUF_SIZE];
	struct page *p_print;
	unsigned char *px;

    pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
    pte_t *ptep, pte;
	old_fs = get_fs();
	set_fs(KERNEL_DS);

    printk("slave device ioctl");

	switch(ioctl_num){
		case slave_IOCTL_CREATESOCK:// create socket and connect to master
            printk("slave device ioctl create socket");

			if(copy_from_user(ip, (char*)ioctl_param, sizeof(ip)))
				return -ENOMEM;

			sprintf(current->comm, "ksktcli");

			memset(&addr_srv, 0, sizeof(addr_srv));
			addr_srv.sin_family = AF_INET;
			// Note: 2325 is defined as DEFAULT_PORT in master_device.c
			addr_srv.sin_port = htons(2325);
			addr_srv.sin_addr.s_addr = inet_addr(ip);
			addr_len = sizeof(struct sockaddr_in);

			sockfd_cli = ksocket(AF_INET, SOCK_STREAM, 0);
			printk("sockfd_cli = 0x%p  socket is created\n", sockfd_cli);
			if (sockfd_cli == NULL)
			{
				printk("socket failed\n");
				return -1;
			}
			if (kconnect(sockfd_cli, (struct sockaddr*)&addr_srv, addr_len) < 0)
			{
				printk("connect failed\n");
				return -1;
			}
			tmp = inet_ntoa(&addr_srv.sin_addr);
			printk("connected to : %s %d\n", tmp, ntohs(addr_srv.sin_port));
			kfree(tmp);
			printk("kfree(tmp)");
			ret = 0;
			break;
		case slave_IOCTL_MMAP:
			// Note: Use krecv anyway.
			len = krecv(sockfd_cli, file->private_data, 4096, 0);
			break;

		case slave_IOCTL_EXIT:
			if(kclose(sockfd_cli) == -1)
			{
				printk("kclose cli error\n");
				return -1;
			}
			ret = 0;
			break;
		default:
            pgd = pgd_offset(current->mm, ioctl_param);
			p4d = p4d_offset(pgd, ioctl_param);
			pud = pud_offset(p4d, ioctl_param);
			pmd = pmd_offset(pud, ioctl_param);
			ptep = pte_offset_kernel(pmd , ioctl_param);
			pte = *ptep;
			printk("slave: %lX\n", pte);
			ret = 0;
			break;
	}
    set_fs(old_fs);

	return ret;
}

ssize_t receive_msg(struct file *filp, char *buf, size_t count, loff_t *offp )
{
//call when user is reading from this device
	char msg[BUF_SIZE];
	size_t len;
	len = krecv(sockfd_cli, msg, sizeof(msg), 0);
	if(copy_to_user(buf, msg, len))
		return -ENOMEM;
	return len;
}




module_init(slave_init);
module_exit(slave_exit);
MODULE_LICENSE("GPL");
