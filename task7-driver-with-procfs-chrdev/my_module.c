// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h> // Macros used to mark up functions e.g., __init __exit
#include <linux/module.h> // Core header for loading LKMs into the kernel
#include <linux/kernel.h> // Contains types, macros, functions for the kernel
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>

MODULE_LICENSE("GPL"); ///< The license type -- this affects runtime behavior
MODULE_AUTHOR("Maksym Khomenko"); ///< The author -- visible when you use modinfo
MODULE_DESCRIPTION("Simple gpio driver with proc_fs support"); ///< The description -- see modinfo
MODULE_VERSION("0.2"); ///< The version of the module

#define GPIO_4 (4)		// LED connected to this gpio
#define GPIO_23 (23)	// Button connected to this gpio
#define TIMEOUT 2000    // 2000 ms
#define BUTTON_SCAN_PERIOD 100 // 100 ms
#define PROC_BUFFER_SIZE 256

// Character device defines
#define CLASS_NAME "chrdev"
#define DEVICE_NAME "my_chrdevice"
#define DATA_BUFFER_SIZE 1024

static char procfs_buffer[PROC_BUFFER_SIZE] = "MODULE NAME: my_module\nAUTHOR: Maksym Khomenko\nDESCRIPTION: Simple gpio character driver with proc_fs support\nVERSION: 0.3\nLED STATE: ON\n";
static size_t procfs_buffer_size = PROC_BUFFER_SIZE;
static struct proc_dir_entry *proc_file;
static struct proc_dir_entry *proc_folder;

// Character device related variables
static struct class *pclass;
static struct device *pdev;
static struct cdev chrdev_cdev;
dev_t dev = 0;
static int major;
static int is_open;
static int data_size;
static unsigned char data_buffer[DATA_BUFFER_SIZE];

static struct timer_list etx_timer;		// LED blink timer
static struct timer_list b_timer;		// Button scan timer

static u8 blink = 1;					// LED state (1 - blink, 0 - not blink)

#define PROC_FILE_NAME "my_info"
#define PROC_DIR_NAME "my_module" /* /proc/my_module/my_state */


static int dev_open(struct inode *inodep, struct file *filep)
{
	if (is_open)
	{
		pr_err("MY_MODULE: chardev already open\n");
		return -EBUSY;
	}
	is_open = 1;
	pr_info("MY_MODULE: chardev opened\n");
	return 0;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
	is_open = 0;
	pr_info("MY_MODULE: chardev closed\n");
	return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	int ret;
	pr_info("MY_MODULE: chardev read from file %s\n", filep->f_path.dentry->d_iname);
	pr_info("MY_MODULE: chardev read from device %d:%d\n", imajor(filep->f_inode), iminor(filep->f_inode));

	if (len > data_size) len = data_size;
	ret = copy_to_user(buffer, data_buffer, len);
	if (ret)
	{
		pr_err("MY_MODULE: chardev copy_to_user failed %d\n", ret);
		return -EFAULT; 
	}

	data_size = 0;
	pr_info("MY_MODULE: chardev %lu bytes read\n", len);
	return len;
}


static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
	int ret;
	pr_info("MY_MODULE: chardev write to file %s\n", filep->f_path.dentry->d_iname);
	pr_info("MY_MODULE: chardev write to device %d:%d\n", imajor(filep->f_inode), iminor(filep->f_inode));

	data_size = len;
	if (data_size > DATA_BUFFER_SIZE) data_size = DATA_BUFFER_SIZE;
	ret = copy_from_user(data_buffer, buffer, data_size);
	if (ret)
	{
		pr_err("MY_MODULE: chardev copy_from_user failed %d\n", ret);
		return -EFAULT; 
	}
	pr_info("MY_MODULE: chardev %d bytes written\n", data_size);
	return data_size;
}


static int my_dev_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

static struct file_operations cdev_fops =
{
	.open = dev_open,
	.release = dev_release,
	.read = dev_read,
	.write = dev_write
};


static ssize_t my_module_read(struct file *File, char __user* buffer, size_t count, loff_t *offset)
{
	ssize_t to_copy, not_copied, delta;

	if (procfs_buffer_size == 0) 
		{
			procfs_buffer_size = strlen(procfs_buffer);
			return 0;
		}

	to_copy = min(count, procfs_buffer_size);
	not_copied = copy_to_user(buffer, procfs_buffer, to_copy);
	delta = to_copy - not_copied;
	procfs_buffer_size -= delta;
	return delta;
}

// LED blink timer callback
void timer_callback(struct timer_list *data)
{
	char *position_ptr = strstr(procfs_buffer, "LED STATE: ");
	pr_info_once("MY_MODULE: Timer %s()\n", __func__);
	if (blink)
	{
		if (gpio_get_value(GPIO_4))
		{
			gpio_set_value(GPIO_4, 0);
			strcpy((position_ptr + 11), "OFF\n");
		}
		else
		{
			gpio_set_value(GPIO_4, 1);
			strcpy((position_ptr + 11), "ON\n");
		}
	}
	mod_timer(&etx_timer, jiffies + msecs_to_jiffies(TIMEOUT));
}

// Button scan timer callback
void b_timer_callback(struct timer_list *data)
{
	static u8 b_prev_state = 1;
	u8 b_curr_state = gpio_get_value(GPIO_23);
	if (!b_curr_state  && b_prev_state)
	{
		blink = !blink;
	}
	b_prev_state = b_curr_state;
	pr_info_once("MY_MODULE: Timer %s()\n", __func__);
	mod_timer(&b_timer, jiffies + msecs_to_jiffies(BUTTON_SCAN_PERIOD));
}

//static struct file_operations fops = {.read = my_module_read};
static struct proc_ops fops = {.proc_read = my_module_read};


static int __init my_module_init(void)
{
	int ret = 0;

	is_open = 0;
	data_size = 0;

	major = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if(major < 0)
	{
		pr_err("MY_MODULE: chardev register_chardev failed %d\n", major);
		return major;
	}
	pr_info("MY_MODULE: chardev egister_chardev ok, major = %d minor = %d\n", MAJOR(dev), MINOR(dev));

	cdev_init(&chrdev_cdev, &cdev_fops);
	if(cdev_add(&chrdev_cdev, dev, 1) < 0)
	{
		pr_err("MY_MODULE: chardev cannot add the device to the system\n");
		ret = -1;
		goto cdev_err;
	}
	pr_info("MY_MODULE: chardev cdev created successfully\n");

	pclass = class_create(THIS_MODULE, CLASS_NAME);
	if(IS_ERR(pclass))
	{
		ret = -1;
		goto class_err;
	}
	pclass->dev_uevent = my_dev_uevent;
	pr_info("MY_MODULE: chardev device class created successfully\n");

	pdev = device_create(pclass, NULL, dev, NULL, CLASS_NAME"0");
	if(IS_ERR(pdev))
	{
		ret = -1;
		goto device_err;
	}
	pr_info("MY_MODULE: chardev device node created successfully\n");

	if (gpio_is_valid(GPIO_4) == false)
	{
		pr_err("MY_MODULE: GPIO %d is not valid\n", GPIO_4);
		ret = -1;
		goto device_err;
	}
	if (gpio_is_valid(GPIO_23) == false)
	{
		pr_err("MY_MODULE: GPIO %d is not valid\n", GPIO_23);
		ret = -1;
		goto device_err;
	}

	if (gpio_request(GPIO_4, "GPIO_4") < 0)
	{
		pr_err("MY_MODULE: Error GPIO %d request\n", GPIO_4);
		ret = -1;
		goto device_err;
	}
	if (gpio_request(GPIO_23, "GPIO_23") < 0)
	{
		pr_err("MY_MODULE: Error GPIO %d request\n", GPIO_23);
		ret = -1;
		goto device_err;
	}

	gpio_direction_output(GPIO_4, 0);
	gpio_direction_input(GPIO_23);

	pr_info("MY_MODULE: GPIO %d init done\n", GPIO_4);
	pr_info("MY_MODULE: GPIO %d init done\n", GPIO_23);
	gpio_set_value(GPIO_4, 1);

	proc_folder = proc_mkdir(PROC_DIR_NAME, NULL);
	if(!proc_folder)
	{
		pr_err("MY_MODULE: Error Could not create /proc/%s/ folder\n", PROC_DIR_NAME);
		gpio_free(GPIO_4);
		gpio_free(GPIO_23);
		ret = -ENOMEM;
		goto device_err;
	}
	proc_file = proc_create(PROC_FILE_NAME, 0666, proc_folder, &fops);
	if(!proc_file)
	{
		pr_err("MY_MODULE: Error Could not initialize /proc/%s/%s\n", PROC_DIR_NAME, PROC_FILE_NAME);
		proc_remove(proc_file);
		proc_remove(proc_folder);
		gpio_free(GPIO_4);
		gpio_free(GPIO_23);
		ret = -ENOMEM;
		goto device_err;
	}
	procfs_buffer_size = strlen(procfs_buffer);
	pr_info("MY_MODULE: /proc/%s/%s created\n", PROC_DIR_NAME, PROC_FILE_NAME);
	timer_setup(&etx_timer, timer_callback, 0);
	timer_setup(&b_timer, b_timer_callback, 0);
	mod_timer(&etx_timer, jiffies + msecs_to_jiffies(TIMEOUT));
	mod_timer(&b_timer, jiffies + msecs_to_jiffies(BUTTON_SCAN_PERIOD));
	pr_info("MY_MODULE: Timer %s()\n", __func__);
	return ret;
device_err:
	class_destroy(pclass);
class_err:
	cdev_del(&chrdev_cdev);
cdev_err:
	unregister_chrdev_region(dev, 1);
	return ret;
}

static void __exit my_module_exit(void)
{
	device_destroy(pclass, dev);
	class_destroy(pclass);
	cdev_del(&chrdev_cdev);
	unregister_chrdev_region(dev, 1);
	pr_info("MY_MODULE: chardev module deleted\n");
	del_timer(&etx_timer);
	del_timer(&b_timer);
	pr_info("MY_MODULE: Timer %s() deleted\n", __func__ );
	gpio_set_value(GPIO_4, 0);
	gpio_free(GPIO_4);
	gpio_free(GPIO_23);
	pr_info("MY_MODULE: GPIO free done\n");
	proc_remove(proc_file);
	proc_remove(proc_folder);
	pr_info("MY_MODULE: /proc/%s/%s removed\n", PROC_DIR_NAME, PROC_FILE_NAME);
}

module_init(my_module_init);
module_exit(my_module_exit);
