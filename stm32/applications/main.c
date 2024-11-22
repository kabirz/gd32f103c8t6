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

#define PWR_CTL_IN_PINT GET_PIN(B, 15)
#define PWR_CTL_OUT_PINT GET_PIN(B, 13)

enum {
    PWR_PIN_LOW = 1,
    PWR_PIN_HIGH
} pin_status;

int main(void)
{
    rt_uint32_t event_set = 0;
    rt_tick_t tick_1 = 0, tick_2 = 0;

    rt_event_init(&event, "power_event", RT_IPC_FLAG_PRIO);

    rt_pin_mode(PWR_CTL_IN_PINT, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_mode(PWR_CTL_OUT_PINT, PIN_MODE_OUTPUT);
    rt_pin_attach_irq(PWR_CTL_IN_PINT, PIN_IRQ_MODE_RISING_FALLING, power_in_isr, RT_NULL);
    rt_pin_irq_enable(PWR_CTL_IN_PINT, PIN_IRQ_ENABLE);

    mb_rtu_setup();


    while (1) {
    	rt_event_recv(&event, PWR_PIN_LOW | PWR_PIN_HIGH, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &event_set);
    	switch (event_set) {
    	case PWR_PIN_HIGH:
    	    tick_1 = rt_tick_get();
    	    break;
    	case PWR_PIN_LOW:
    	    tick_2 = rt_tick_get();
    	    if (tick_2 - tick_1 > 100 && tick_2 - tick_1 < 500) {
                power_reset();
    	    }
    	default:
    	    tick_1 = 0;
    	    tick_2 = 0;
    	    break;
    	}
    }

    return RT_EOK;
}

static void power_in_isr(void *args)
{
    rt_interrupt_enter();
    rt_size_t level = rt_pin_read(PWR_CTL_IN_PINT);
    rt_event_send(&event, level == PIN_LOW ? PWR_PIN_LOW : PWR_PIN_HIGH);
    rt_interrupt_leave();
}


#define UART_POART 2

enum {
    HOLDING_PWR_LEVEL = 0, /* 0: low, 1: high */
    HOLDING_PWR_STATUS, /* 0: power off, 1: power on */
    HOLDING_PWR_DELAY_WAIT, /* delay time for wait */
    HOLDING_PWR_DELAY_ON, /* delay time for power on */
    HOLDING_PWR_RESET,  /* 0: no reset, 1: reset */
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
    rt_pin_write(PWR_CTL_OUT_PINT, PIN_LOW);
    rt_thread_mdelay(mb_datas.holding_buf[HOLDING_PWR_DELAY_ON]);
    rt_pin_write(PWR_CTL_OUT_PINT, PIN_HIGH);
}


static void holding_handler(void)
{
    bool changed = false;

    for (int i = 0; i < S_REG_HOLDING_NREGS; i++) {
        if (mb_datas.holding_buf[i] == HOLDING_REGS[i])
            continue;
        switch (i) {
        case HOLDING_PWR_RESET:
            power_reset();
            HOLDING_REGS[i] = 0;
            break;
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
            rt_kprintf("save holding regs to flash failed!");
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
    mb_datas.holding_buf[HOLDING_PWR_LEVEL] = 1;
    mb_datas.holding_buf[HOLDING_PWR_STATUS] = 1;
    mb_datas.holding_buf[HOLDING_PWR_DELAY_WAIT] = 100;
    mb_datas.holding_buf[HOLDING_PWR_DELAY_ON] = 200;
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
            rt_kprintf("magic error!");
            set_mb_cfg_default();
    	} else {
            int crc16 = usMBCRC16((UCHAR *)mb_datas.holding_buf, sizeof(mb_datas.holding_buf));
            if (crc16 != mb_datas.crc16) {
            	rt_kprintf("crc16 error!");
            	set_mb_cfg_default();
            }
        }
    }

    /* init */
    mb_datas.holding_buf[HOLDING_PWR_RESET] = 0;
    rt_memcpy((void *)HOLDING_REGS, mb_datas.holding_buf, sizeof(mb_datas.holding_buf));
    if (mb_datas.holding_buf[HOLDING_PWR_STATUS])
        rt_pin_write(PWR_CTL_OUT_PINT, mb_datas.holding_buf[HOLDING_PWR_LEVEL] == 0 ? PIN_LOW : PIN_HIGH);
    else
        rt_pin_write(PWR_CTL_OUT_PINT, PIN_LOW);


    rt_thread_t tid1 = RT_NULL;

    tid1 = rt_thread_create("mbs_rtu", mb_rtu_poll, NULL, 2048, 12, 10);
    if (tid1 != RT_NULL) {
        rt_thread_startup(tid1);
        return RT_EOK;
    }

    return -RT_ERROR;
}

