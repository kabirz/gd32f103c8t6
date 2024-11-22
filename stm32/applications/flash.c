#include <rtthread.h>
#include <drv_flash.h>

#define FMC_PAGE_SIZE           1024
#define FMC_WRITE_START_ADDR    0x0800fc00

int flash_write(const void *data, size_t data_len)
{
    if (data_len > FMC_PAGE_SIZE) {
    	rt_kprintf("data_len is too large\n");
    	return -1;
    }

    /* erase */
    stm32_flash_erase(FMC_WRITE_START_ADDR, FMC_PAGE_SIZE);
    /* write */
    stm32_flash_write(FMC_WRITE_START_ADDR, data, data_len);

    return 0;
}

int flash_read(void *data, size_t data_len)
{
    if (data_len > FMC_PAGE_SIZE) {
    	rt_kprintf("data_len is too large\n");
    	return -1;
    }

    /* read flash */
    rt_memcpy(data, (void *)FMC_WRITE_START_ADDR, data_len);

    return 0;
}

