#include <rtthread.h>
#include <stdbool.h>
#include <rtdevice.h>
#include <board.h>
#include <drv_gpio.h>
#include <user_mb_app.h>

static int mb_rtu_setup(void);
static void power_reset(void);
static void power_in_isr(void *args);
static struct rt_event event;

#define PWR_CTL_IN_PIN GET_PIN(B, 15)
#define PWR_CTL_OUT_PIN GET_PIN(B, 13)
#define POWER_RESET 1

bool skip_irq;
rt_tick_t tick;

int main(void)
{
    rt_uint32_t event_set = 0;
    rt_tick_t tick_1 = 0, tick_2 = 0;

    rt_event_init(&event, "power_event", RT_IPC_FLAG_PRIO);

    rt_pin_mode(PWR_CTL_IN_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_mode(PWR_CTL_OUT_PIN, PIN_MODE_OUTPUT);
    rt_pin_attach_irq(PWR_CTL_IN_PIN, PIN_IRQ_MODE_RISING_FALLING, power_in_isr, RT_NULL);
    rt_pin_irq_enable(PWR_CTL_IN_PIN, PIN_IRQ_ENABLE);

    mb_rtu_setup();
    rt_kprintf("stm32f103c8t6, sysclock: %dMhz\n", SystemCoreClock/1000000);


    while (1) {
    	rt_event_recv(&event, POWER_RESET, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &event_set);
    	skip_irq = true;
    	switch (event_set) {
    	case POWER_RESET:
    	    rt_thread_mdelay(20); /* debouncing delay */
    	    if (rt_pin_read(PWR_CTL_IN_PIN)) {
    	    	tick_1 = tick;
    	    	rt_kprintf("l->h:[%d] ticks: %d\n", rt_pin_read(PWR_CTL_IN_PIN), tick_1);
    	    } else {
    	    	tick_2 = tick;
    	    	rt_kprintf("h->l:[%d] ticks: %d, diff: %d\n", rt_pin_read(PWR_CTL_IN_PIN), tick_2, tick_2 - tick_1);
    	    	if (tick_2 - tick_1 > 100 && tick_2 - tick_1 < 500) {
    	    	    power_reset();
    	    	}
    	    	tick_1 = tick_2 = 0;
    	    }
    	default:
    	    break;
    	}
    	skip_irq = false;
    }

    return RT_EOK;
}

static void power_in_isr(void *args)
{
    rt_interrupt_enter();
    if (!skip_irq) {
    	tick = rt_tick_get();
    	rt_event_send(&event, POWER_RESET);
    }
    rt_interrupt_leave();
}


#define UART_POART 2

enum {
    HOLDING_PWR_LEVEL = 0, /* 0: low, 1: high */
    HOLDING_PWR_STATUS, /* 0: power off, 1: power on */
    HOLDING_PWR_DELAY_WAIT, /* delay time for wait */
    HOLDING_PWR_DELAY_ON, /* delay time for power on */
    HOLDING_PWR_RESET,  /* 1: power status reset */
    HOLDING_SYS_REBOOT,  /* 1: board reboot */
};

#define HOLDING_REGS ((volatile uint16_t*)&modbus_reg_bufs[S_REG_HOLDING_START])
extern int flash_write(const void *data, size_t data_len);
extern int flash_read(void *data, size_t data_len);
/* this function from packages/freemodbus-latest/modbus/rtu/mbcrc.c */
extern USHORT usMBCRC16(UCHAR * pucFrame, USHORT usLen);

static struct {
    uint16_t crc16;
    uint16_t magic;
    uint16_t holding_buf[S_REG_HOLDING_NREGS];
} mb_datas;

static void power_reset(void)
{
    rt_thread_mdelay(mb_datas.holding_buf[HOLDING_PWR_DELAY_WAIT]);
    rt_kprintf("power reset......\n");
    rt_pin_write(PWR_CTL_OUT_PIN, !mb_datas.holding_buf[HOLDING_PWR_LEVEL]);
    rt_thread_mdelay(mb_datas.holding_buf[HOLDING_PWR_DELAY_ON]);
    rt_pin_write(PWR_CTL_OUT_PIN, mb_datas.holding_buf[HOLDING_PWR_LEVEL]);
}


static void holding_handler(void)
{
    bool changed = false;

    for (int i = 0; i < S_REG_HOLDING_NREGS; i++) {
        if (mb_datas.holding_buf[i] == HOLDING_REGS[i])
            continue;
        switch (i) {
        case HOLDING_PWR_RESET:
            if (HOLDING_REGS[i] == 1)
                power_reset();
            HOLDING_REGS[i] = 0;
            break;
        case HOLDING_SYS_REBOOT:
            if (HOLDING_REGS[i] == 1)
                rt_hw_cpu_reset();
            break;
        case HOLDING_PWR_LEVEL:
        case HOLDING_PWR_STATUS:
            HOLDING_REGS[i] = !!HOLDING_REGS[i];
        default:
            changed = true;
            break;
        }
    }

    if (changed) {
        rt_memcpy(mb_datas.holding_buf, (void *)HOLDING_REGS, sizeof(mb_datas.holding_buf));

        /* save to flash */
        mb_datas.crc16 = usMBCRC16((UCHAR *)mb_datas.holding_buf, sizeof(mb_datas.holding_buf));
        if (flash_write(&mb_datas, sizeof(mb_datas))) {
            rt_kprintf("save holding regs to flash failed!\n");
        }
    }

}

static void mb_rtu_poll(void *parameter)
{
    eMBInit(MB_RTU, 1, UART_POART, 9600, 0);
    eMBEnable(MB_RTU);
    while (1) {
        eMBPoll(MB_RTU);
        switch (eMBGetHoldingChange()) {
        case 1:
            holding_handler();
            break;
        default:
            break;
        }
    }
}

static void set_mb_cfg_default(void)
{
    rt_memset(&mb_datas, 0, sizeof(mb_datas));
    mb_datas.magic = 0xdead;
    mb_datas.holding_buf[HOLDING_PWR_LEVEL] = 0;
    mb_datas.holding_buf[HOLDING_PWR_STATUS] = 1;
    mb_datas.holding_buf[HOLDING_PWR_DELAY_WAIT] = 3000;
    mb_datas.holding_buf[HOLDING_PWR_DELAY_ON] = 3000;
}

static int mb_rtu_setup(void)
{
    /* read config from flash */
    int ret = flash_read(&mb_datas, sizeof(mb_datas));
    if (ret) {
        rt_kprintf("read holding regs from flash failed!\n");
        set_mb_cfg_default();
    } else {
    	if (mb_datas.magic != 0xdead) {
            rt_kprintf("magic error!\n");
            set_mb_cfg_default();
    	} else {
            int crc16 = usMBCRC16((UCHAR *)mb_datas.holding_buf, sizeof(mb_datas.holding_buf));
            if (crc16 != mb_datas.crc16) {
            	rt_kprintf("crc16 error!\n");
            	set_mb_cfg_default();
            }
        }
    }

    /* init */
    mb_datas.holding_buf[HOLDING_PWR_RESET] = 0;
    rt_memcpy((void *)HOLDING_REGS, mb_datas.holding_buf, sizeof(mb_datas.holding_buf));
    if (mb_datas.holding_buf[HOLDING_PWR_STATUS])
        rt_pin_write(PWR_CTL_OUT_PIN, mb_datas.holding_buf[HOLDING_PWR_LEVEL]);
    else
        rt_pin_write(PWR_CTL_OUT_PIN, !mb_datas.holding_buf[HOLDING_PWR_LEVEL]);


    rt_thread_t tid1 = RT_NULL;

    tid1 = rt_thread_create("mbs_rtu", mb_rtu_poll, NULL, 2048, 12, 10);
    if (tid1 != RT_NULL) {
        rt_thread_startup(tid1);
        return RT_EOK;
    }

    return -RT_ERROR;
}

