#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#define PWR_CTL_IN_PINT GET_PIN(B, 15)
#define PWR_CTL_OUT_PINT GET_PIN(B, 13)

enum {
    PWR_PIN_LOW = 1,
    PWR_PIN_HIGH
} pin_status;

static struct rt_event event;
static void power_in_isr(void *args)
{
    rt_size_t level = rt_pin_read(PWR_CTL_IN_PINT);
    rt_event_send(&event, level == PIN_LOW ? PWR_PIN_LOW : PWR_PIN_HIGH);
}

void power_reset(void)
{
    rt_pin_write(PWR_CTL_OUT_PINT, PIN_LOW);
    rt_thread_mdelay(100);
    rt_pin_write(PWR_CTL_OUT_PINT, PIN_HIGH);
}

int main(void)
{
    rt_uint32_t event_set = 0;
    rt_tick_t tick_h = 0, tick_l = 0;

    rt_pin_mode(PWR_CTL_IN_PINT, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_mode(PWR_CTL_OUT_PINT, PIN_MODE_OUTPUT);
    rt_pin_attach_irq(PWR_CTL_IN_PINT, PIN_IRQ_MODE_RISING_FALLING, power_in_isr, RT_NULL);
    rt_pin_irq_enable(PWR_CTL_IN_PINT, PIN_IRQ_ENABLE);

    rt_event_init(&event, "power_event", RT_IPC_FLAG_PRIO);

    while (1) {
    	rt_event_recv(&event, PWR_PIN_LOW | PWR_PIN_HIGH, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &event_set);
    	switch (event_set) {
    	case PWR_PIN_LOW:
    	    tick_l = rt_tick_get();
    	    break;
    	case PWR_PIN_HIGH:
    	    tick_h = rt_tick_get();
    	    if (tick_h - tick_l > 100 || tick_h - tick_l > 500) {
    	    	power_reset();
    	    }
    	default:
    	    tick_l = 0;
    	    tick_h = 0;
    	    break;
    	}
    }

    return RT_EOK;
}
