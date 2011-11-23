/*
    Copyright (C) Chris Desjardins 2008 - cjd@chrisd.info

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
    Just a quick linux device driver: 
    Servos have hard real time requirements, if you are too early or too late
    on your deadlines, then the servos will twitch. Servos use PWM to control
    the angle, the width of the pulse determines the angle of the servo.
    If you have a servo that is being driven with a pulsewidth of 1300 microseconds
    that means that every 20 milliseconds you must drive the pulse high for 
    exactly 1300 microseconds. If you drive it high for 1400 microseconds, then
    the servo will move, if you drive it high for 1200 microseconds then the
    servo will move in the other direction. But if you constantly put a 1300 
    microsecond pulse on your servo every 20 milliseconds, then it will not move 
    at all and will be driven to the exact angle you desire!

    Furthermore if you do not start a new pulse every 20 milliseconds then the
    servo can go limp and will not be driven to the desired angle.

    Because this code was developed with a non-realtime OS (Linux) I had to fight
    to make it work properly. The accomplish this I had to consume a considerable 
    amount of CPU time in order to prevent the Linux schedule from allowing some
    other task to consume the CPU and cause the servo driver to miss deadlines.

    This is ok if you don't really plan to do anything else with your CPU, but
    if you need to perform other processing then you should consider implementing
    the servo driver in a RTOS, or FPGA or something like that.
*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include "asm/arch/mux.h"
#include <linux/gpio.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/list.h>
#include "../../servosoc.h"

MODULE_LICENSE("Dual BSD/GPL");

#define SERVODRIVE_GPIO_BASE        136
#define SERVODRIVE_NAME             "servoctrldrv"
#define SERVODRIVE_PERIOD           20000

int servodrive_open(struct inode *inode, struct file *filp);
int servodrive_release(struct inode *inode, struct file *filp);
ssize_t servodrive_read(struct file *filp, char *buf, size_t count, loff_t *f_pos);
ssize_t servodrive_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos);

struct file_operations servodrive_funcs = {
    read: servodrive_read,
    write: servodrive_write,
    open: servodrive_open,
    release: servodrive_release
};

typedef struct
{
    int m_nGpio;
    unsigned long m_nDelay;
} TServoDrvData;

int g_nServoDrvMajor = 0;
int g_nServoDrvOpen = 0;
struct timer_list g_tlServoDrvPwm;
TServoDrvData g_ServoDrvList[SERVOSOC_MAX_SERVOS];


void servodrive_pwm(unsigned long arg);

/*
    Init the mux on the beagle board omap processor.
*/
static int servodrive_init_mux(int mux_start_index, int mux_end_index)
{
    int i;
    int ret = 0;
    for (i = mux_start_index; i <= mux_end_index; i++)
    {
        ret = omap_cfg_reg(i);
        if (ret != 0)
        {
            printk("<1>Servodrive: omap_cfg_reg failed\n");
            break;
        }
    }
    return ret;
}

/*
    Init the GPIOs connected to the servos.
*/
static int servodrive_init_data(void)
{
    int i;
    for (i = 0; i < SERVOSOC_MAX_SERVOS; i++)
    {
        gpio_direction_output(i + SERVODRIVE_GPIO_BASE, 0);
        g_ServoDrvList[i].m_nGpio   = -1;
        g_ServoDrvList[i].m_nDelay  = 0;
    }
    return 0;
}

/*
    Do all major linux device driver init stuff.
*/
static int servodrive_init(void) 
{
    int ret;
    printk("<1>Starting servo drive %s %s\n", __DATE__, __TIME__);

    ret = servodrive_init_mux(XXXX_3430_GPIO_136, XXXX_3430_GPIO_143);
    if (ret != 0)
    {
        return ret;
    }
    
    ret = servodrive_init_data();
    if (ret != 0)
    {
        return ret;
    }
    g_nServoDrvMajor = register_chrdev(g_nServoDrvMajor, SERVODRIVE_NAME, &servodrive_funcs);
    if (g_nServoDrvMajor < 0)
    {
        printk("<l>Servodrive error: %i\n", g_nServoDrvMajor);
        return g_nServoDrvMajor;
    }
    printk("<1>major=%i\n", g_nServoDrvMajor);

    return 0;
}

/*
    Handle open calls to this driver.
*/
int servodrive_open(struct inode *inode, struct file *filp)
{
    printk("<1>Servoctrldrv open\n");
    g_nServoDrvOpen = 1;
    init_timer(&g_tlServoDrvPwm);
    g_tlServoDrvPwm.function = servodrive_pwm;
    g_tlServoDrvPwm.expires = jiffies + usecs_to_jiffies(SERVODRIVE_PERIOD);
    add_timer(&g_tlServoDrvPwm);

    return 0;
}

/*
    This is a write only driver!
*/
ssize_t servodrive_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
    return 0;
}

/*
    Handle writes to a servo which cause the servo to change position, in this
    case only 2 servos are supported and their signaling lines are connected
    to GPIOs 137 and 139.
*/
ssize_t servodrive_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos)
{
    int nServoIndex;
    TServoData sServoList[SERVOSOC_MAX_SERVOS];

    memset(sServoList, 0, sizeof(sServoList));
    if (copy_from_user(sServoList, buf, count))
    {
        printk("<1>Servodrive: write error\n");
        return -EFAULT;
    }
    for (nServoIndex = 0; nServoIndex < SERVOSOC_MAX_SERVOS; nServoIndex++)
    {
        switch (sServoList[nServoIndex].m_nServoNumber)
        {
        case SERVOSOC_SERVO_A:
            g_ServoDrvList[nServoIndex].m_nGpio = 139;
            break;
        case SERVOSOC_SERVO_B:
            g_ServoDrvList[nServoIndex].m_nGpio = 137;
            break;
        case SERVOSOC_SERVO_INVALID:
            g_ServoDrvList[nServoIndex].m_nGpio = -1;
            break;
        default:
            printk("<1> I dont even have this line hooked up to hardware, give me a break!\n");
            break;
        }
        if (g_ServoDrvList[nServoIndex].m_nGpio != -1)
        {
            g_ServoDrvList[nServoIndex].m_nDelay = sServoList[nServoIndex].m_nJoyValue;
        }
    }
    *f_pos += count;
    return count;
}

/*
    Close down the servo driver.
*/
int servodrive_release(struct inode *inode, struct file *filp)
{
    g_nServoDrvOpen = 0;
    del_timer_sync(&g_tlServoDrvPwm);
    return 0;
}

/*
    Drive a pulse to the servos, the algorithm is as follows:
    1) Drive all servo signaling pins high.
    2) Wait until the servo with the shortest pulse is done and drop that pin low.
    3) Wait the remaining time necessary for the next shortest servo pulse and drop that pin low.
    4) Repeat step 3 until all servos are serviced.
    5) Wait (20ms - pulse width of the longest pulse) and repeat the whole process again.
    Note: This is function is expensive for what it does, in the worst case this function 
    will hog the CPU for about 2ms every 20ms.
*/
void servodrive_pwm(unsigned long arg)
{
    /* restart this timer for the next period */
    int i;
    unsigned long nTotalDelay = 0;
    int nServos = 0;
    if (g_nServoDrvOpen == 1)
    {
        g_tlServoDrvPwm.expires = jiffies + usecs_to_jiffies(SERVODRIVE_PERIOD);
        add_timer(&g_tlServoDrvPwm);
    }
    for (i = 0; i < SERVOSOC_MAX_SERVOS; i++)
    {
        if (g_ServoDrvList[i].m_nGpio != -1)
        {
            gpio_set_value(g_ServoDrvList[i].m_nGpio, 1);
        }
        else
        {
            break;
        }
        nServos++;
    }
    for (i = 0; i < nServos; i++)
    {
        if (g_ServoDrvList[i].m_nGpio != -1)
        {
            udelay(g_ServoDrvList[i].m_nDelay - nTotalDelay);
            nTotalDelay += g_ServoDrvList[i].m_nDelay;
            gpio_set_value(g_ServoDrvList[i].m_nGpio, 0);
        }
    }
}

static void servodrive_exit(void) 
{
    printk("<1>Exiting servodrive\n");
    unregister_chrdev(g_nServoDrvMajor, SERVODRIVE_NAME);
}

module_init(servodrive_init);
module_exit(servodrive_exit);
