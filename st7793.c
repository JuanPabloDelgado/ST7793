/*
 * ST7793 LCD for Raspberry Pi Zero 2W in 8-bit mode
 */

#define BCM2708_PERI_BASE 0xFE000000 /* For RPI Zero 2W - for older models this may change */
#define GPIO_BASE (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#define BLOCKSIZE (4 * 1024)

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio + ((g) / 10)) &= ~(7 << (((g) % 10) * 3))
#define OUT_GPIO(g) *(gpio + ((g) / 10)) |= (1 << (((g) % 10) * 3))
#define SET_GPIO_ALT(g, a) *(gpio + (((g) / 10))) |= (((a) <= 3 ? (a) + 4 : (a) == 4 ? 3 : 2) << (((g) % 10) * 3))

#define GPIO_SET *(gpio + 7)  // sets bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio + 10) // clears bits which are 1 ignores bits which are 0

// GPIO pins for data lines (8-bit mode)
#define DATA0 4
#define DATA1 5
#define DATA2 6
#define DATA3 7
#define DATA4 8
#define DATA5 9
#define DATA6 10
#define DATA7 11

// GPIO pins for control lines
#define DC 20
#define CS 21
#define RD 22
#define WR 23
#define RESET 24

#define ORIENTATION 0 // 0=LANDSCAPE, 1=PORTRAIT

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 400

#define DISPLAY_BPP 16

volatile unsigned *gpio;

// Set to output
static void gpio_setoutput(char g)
{
    INP_GPIO(g); // must use INP_GPIO before we can use OUT_GPIO
    OUT_GPIO(g);
}

// Set state 1=high 0=low
static void gpio_setstate(char g, char state)
{
    (state) ? (GPIO_SET = 1 << g) : (GPIO_CLR = 1 << g);
}

// initialization of GPIO
static void tft_init_board(struct fb_info *info)
{
    gpio_setoutput(DATA0);
    gpio_setoutput(DATA1);
    gpio_setoutput(DATA2);
    gpio_setoutput(DATA3);
    gpio_setoutput(DATA4);
    gpio_setoutput(DATA5);
    gpio_setoutput(DATA6);
    gpio_setoutput(DATA7);
    
    gpio_setoutput(DC);
    gpio_setoutput(CS);
    gpio_setoutput(RD);
    gpio_setoutput(WR);
    gpio_setoutput(RESET);
    
    gpio_setstate(DATA0, 0);
    gpio_setstate(DATA1, 0);
    gpio_setstate(DATA2, 0);
    gpio_setstate(DATA3, 0);
    gpio_setstate(DATA4, 0);
    gpio_setstate(DATA5, 0);
    gpio_setstate(DATA6, 0);
    gpio_setstate(DATA7, 0);
    
    gpio_setstate(DC, 1);
    gpio_setstate(CS, 0);
    gpio_setstate(RD, 1);
    gpio_setstate(WR, 1);
    gpio_setstate(RESET, 1);
}

// hard reset of the graphic controller and the tft
static void tft_hard_reset(void)
{
    gpio_setstate(RESET, 0);
    msleep(120);
    gpio_setstate(RESET, 1);
    msleep(120);
}

static void gpio_set_parallel_data(unsigned char data)
{
    gpio_setstate(DATA0, ((data >> 0) & 0x01));
    gpio_setstate(DATA1, ((data >> 1) & 0x01));
    gpio_setstate(DATA2, ((data >> 2) & 0x01));
    gpio_setstate(DATA3, ((data >> 3) & 0x01));
    gpio_setstate(DATA4, ((data >> 4) & 0x01));
    gpio_setstate(DATA5, ((data >> 5) & 0x01));
    gpio_setstate(DATA6, ((data >> 6) & 0x01));
    gpio_setstate(DATA7, ((data >> 7) & 0x01));
}

// write command
static void tft_command_write(unsigned char command)
{
    gpio_setstate(DC, 0);
    gpio_set_parallel_data(command);
    gpio_setstate(WR, 0);
    gpio_setstate(WR, 1);
}

// write data
static void tft_data_write(unsigned char data)
{
    gpio_setstate(DC, 1);
    gpio_set_parallel_data(data);
    gpio_setstate(WR, 0);
    gpio_setstate(WR, 1);
}

// initialization of ST7793
static void tft_init(struct fb_info *info)
{
    tft_hard_reset();
    
    // ST7793 initialization sequence
    tft_command_write(0x01); // Software reset
    msleep(120);
    tft_command_write(0x11); // Sleep out
    msleep(120);
    
    tft_command_write(0x3A); // Interface pixel format
    tft_data_write(0x55);    // 16-bit pixel format (5-6-5 RGB)

    // Add more initialization commands as needed based on the ST7793 datasheet

    tft_command_write(0x29); // Display on
}

// write memory to TFT
static void st7793_update_display_area(const struct fb_image *image)
{
    int x, y;
    
    // set column
    (ORIENTATION) ? tft_command_write(0x2B) : tft_command_write(0x2A);
    
    tft_data_write(image->dx >> 8);
    tft_data_write(image->dx);
    
    tft_data_write((image->dx + image->width) >> 8);
    tft_data_write(image->dx + image->width);
    
    // set row
    (ORIENTATION) ? tft_command_write(0x2A) : tft_command_write(0x2B);
    
    tft_data_write(image->dy >> 8);
    tft_data_write(image->dy);
    
    tft_data_write((image->dy + image->height) >> 8);
    tft_data_write(image->dy + image->height);
        
    tft_command_write(0x2C); // Memory Write
    
    if (ORIENTATION == 0) {
        for (y = 0; y < image->width; y++) {
            for (x = 0; x < image->height; x++) {
                tft_data_write(image->data[(image->dx * (2 * image->width)) + (image->dy * 2) + 1]);
                tft_data_write(image->data[(image->dx * (2 * image->width)) + (image->dy * 2) + 2]);
            }
        }
    } else {
        for (y = 0; y < image->width; y++) {
            for (x = 0; x < image->height; x++) {
                tft_data_write(image->data[(image->dx * (2 * image->width)) + (image->dy * 2) + 1]);
                tft_data_write(image->data[(image->dx * (2 * image->width)) + (image->dy * 2) + 2]);
            }
        }
    }
    tft_command_write(0x29); // Display ON
}

// Update the display color area
static void st7793_update_display_color_area(const struct fb_fillrect *rect)
{
    int x, y;
    // set column
    (ORIENTATION) ? tft_command_write(0x2B) : tft_command_write(0x2A);
    
    tft_data_write(rect->dx >> 8);
    tft_data_write(rect->dx);
    
    tft_data_write((rect->dx + rect->width) >> 8);
    tft_data_write(rect->dx + rect->width);
    // set row
    
    (ORIENTATION) ? tft_command_write(0x2A) : tft_command_write(0x2B);
    
    tft_data_write(rect->dy >> 8);
    tft_data_write(rect->dy);
    
    tft_data_write((rect->dy + rect->height) >> 8);
    tft_data_write(rect->dy + rect->height);
        
    tft_command_write(0x2C); // Memory Write
    
    if (ORIENTATION == 0) {
        for (y = 0; y < rect->width; y++) {
            for (x = 0; x < rect->height; x++) {
                tft_data_write(rect->color);
                tft_data_write(rect->color >> 8);
            }
        }
    } else {
        for (y = 0; y < rect->height; y++) {
            for (x = 0; x < rect->width; x++) {
                tft_data_write(rect->color);
                tft_data_write(rect->color >> 8);
            }
        }
    }
    tft_command_write(0x29); // Display ON
}

static void st7793_update_display(const struct fb_info *info)
{
    int x, y;
    
    tft_command_write(0x2C); // Memory Write
    
    if (ORIENTATION == 0) {
        for (y = 0; y < DISPLAY_WIDTH; y++) {
            for (x = 0; x < DISPLAY_HEIGHT; x++) {
                tft_data_write(info->screen_base[(x * (2 * DISPLAY_WIDTH)) + (y * 2) + 1]);
                tft_data_write(info->screen_base[(x * (2 * DISPLAY_WIDTH)) + (y * 2) + 2]);
            }
        }
    } else {
        for (y = (DISPLAY_HEIGHT - 1); y >= 0; y--) {
            for (x = 0; x < DISPLAY_WIDTH; x++) {
                tft_data_write(info->screen_base[(y * (2 * DISPLAY_WIDTH)) + (x * 2) + 1]);
                tft_data_write(info->screen_base[(y * (2 * DISPLAY_WIDTH)) + (x * 2) + 2]);
            }
        }
    }
    tft_command_write(0x29); // Display ON
}

static void st7793_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
    st7793_update_display_color_area(rect);
}

static void st7793_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
    st7793_update_display(info);
}

static void st7793_imageblit(struct fb_info *info, const struct fb_image *image)
{
    st7793_update_display_area(image);
}

static ssize_t st7793_write(struct fb_info *info, const char __user *buf, size_t count, loff_t *ppos)
{
    unsigned long p = *ppos;
    void *dst;
    int err = 0;
    unsigned long total_size;

    if (info->state != FBINFO_STATE_RUNNING)
        return -EPERM;

    total_size = info->screen_size;

    if (total_size == 0)
        total_size = info->fix.smem_len;

    if (p > total_size)
        return -EFBIG;

    if (count > total_size) {
        err = -EFBIG;
        count = total_size;
    }

    if (count + p > total_size) {
        if (!err)
            err = -ENOSPC;

        count = total_size - p;
    }

    dst = (void __force *) (info->screen_base + p);

    if (info->fbops->fb_sync)
        info->fbops->fb_sync(info);

    if (copy_from_user(dst, buf, count))
        err = -EFAULT;

    if (!err)
        *ppos += count;

    st7793_update_display(info);

    return (err) ? err : count;
}

static ssize_t st7793_read(struct fb_info *info, char __user *buf, size_t count, loff_t *ppos)
{
    unsigned long p = *ppos;
    void *dst;
    int err = 0;
    unsigned long total_size;

    if (info->state != FBINFO_STATE_RUNNING)
        return -EPERM;

    total_size = info->screen_size;

    if (total_size == 0)
        total_size = info->fix.smem_len;

    if (p > total_size)
        return -EFBIG;

    if (count > total_size) {
        err = -EFBIG;
        count = total_size;
    }

    if (count + p > total_size) {
        if (!err)
            err = -ENOSPC;

        count = total_size - p;
    }

    dst = (void __force *) (info->screen_base + p);

    if (info->fbops->fb_sync)
        info->fbops->fb_sync(info);

    if (copy_from_user(dst, buf, count))
        err = -EFAULT;

    if (!err)
        *ppos += count;

    return (err) ? err : count;
}

static void st7793_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
    st7793_update_display(info);
}

static struct fb_fix_screeninfo st7793_fix = {
    .id = "st7793",
    .type = FB_TYPE_PACKED_PIXELS,
    .visual = FB_VISUAL_TRUECOLOR,
    .accel = FB_ACCEL_NONE,
    .xpanstep = 0,
    .ypanstep = 0,
    .ywrapstep = 0,
    .line_length = DISPLAY_WIDTH * DISPLAY_BPP / 8,
};

static struct fb_var_screeninfo st7793_var = {
    .width = DISPLAY_WIDTH,
    .height = DISPLAY_HEIGHT,
    .bits_per_pixel = DISPLAY_BPP,
    .xres = DISPLAY_WIDTH,
    .yres = DISPLAY_HEIGHT,
    .xres_virtual = DISPLAY_WIDTH,
    .yres_virtual = DISPLAY_HEIGHT,
    .activate = FB_ACTIVATE_NOW,
    .vmode = FB_VMODE_NONINTERLACED,
    .nonstd = 0,
    .red.offset = 11,
    .red.length = 5,
    .green.offset = 5,
    .green.length = 6,
    .blue.offset = 0,
    .blue.length = 5,
    .transp.offset = 0,
    .transp.length = 0,
};

static struct fb_ops st7793_ops = {
    .owner = THIS_MODULE,
    .fb_read = st7793_read,
    .fb_write = st7793_write,
    .fb_fillrect = st7793_fillrect,
    .fb_copyarea = st7793_copyarea,
    .fb_imageblit = st7793_imageblit,
};

static struct fb_deferred_io st7793_defio = {
    .delay = HZ / 25,
    .deferred_io = st7793_deferred_io,
};

static unsigned int fps;

static int st7793_probe(struct platform_device *pdev)
{
    struct fb_info *info;
    int retval = -ENOMEM;
    int vmem_size;
    unsigned char *vmem;

    vmem_size = st7793_var.width * st7793_var.height * st7793_var.bits_per_pixel / 8;
    vmem = vzalloc(vmem_size);
    if (!vmem) {
        return -ENOMEM;
    }
    memset(vmem, 0, vmem_size);

    info = framebuffer_alloc(0, &pdev->dev);
    if (!info) {
        vfree(vmem);
        return -ENOMEM;
    }

    info->screen_base = (char __force __iomem *)vmem;
    info->fbops = &st7793_ops;
    info->fix = st7793_fix;
    info->fix.smem_start = (unsigned long)vmem;
    info->fix.smem_len = vmem_size;
    info->var = st7793_var;
    info->flags = FBINFO_VIRTFB;

    info->fbdefio = &st7793_defio;
    fb_deferred_io_init(info);

    retval = register_framebuffer(info);
    if (retval < 0) {
        framebuffer_release(info);
        vfree(vmem);
        return retval;
    }

    platform_set_drvdata(pdev, info);

    gpio = ioremap(GPIO_BASE, BLOCKSIZE);
    if (!gpio) {
        printk(KERN_ERR "Failed to map GPIO memory\n");
        return -ENOMEM;
    }

    tft_init_board(info);
    tft_hard_reset();
    tft_init(info);

    printk(KERN_INFO "fb%d: ST7793 LCD framebuffer device\n", info->node);
    return 0;
}

static int st7793_remove(struct platform_device *dev)
{
    struct fb_info *info = platform_get_drvdata(dev);

    if (info) {
        unregister_framebuffer(info);
        fb_deferred_io_cleanup(info);
        vfree((void __force *)info->screen_base);
        iounmap(gpio);
        framebuffer_release(info);
    }
    return 0;
}

static struct platform_driver st7793_driver = {
    .probe = st7793_probe,
    .remove = st7793_remove,
    .driver = {
        .name = "st7793",
    },
};

static struct platform_device *st7793_device;

static int __init st7793_init(void)
{
    int ret = platform_driver_register(&st7793_driver);
    if (0 == ret) {
        st7793_device = platform_device_alloc("st7793", 0);
        if (st7793_device) {
            ret = platform_device_add(st7793_device);
        } else {
            ret = -ENOMEM;
        }
        if (0 != ret) {
            platform_device_put(st7793_device);
            platform_driver_unregister(&st7793_driver);
        }
    }
    return ret;
}

static void __exit st7793_exit(void)
{
    platform_device_unregister(st7793_device);
    platform_driver_unregister(&st7793_driver);
}

module_param(fps, uint, 0);
MODULE_PARM_DESC(fps, "Frames per second (default 25)");

module_init(st7793_init);
module_exit(st7793_exit);

MODULE_DESCRIPTION("ST7793 LCD framebuffer driver");
MODULE_AUTHOR("JuanDelgado");
MODULE_LICENSE("GPL");
