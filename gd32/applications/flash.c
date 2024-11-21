#include <rtthread.h>
#include <gd32f10x_fmc.h>

#define FMC_PAGE_SIZE           1024
#define FMC_WRITE_START_ADDR    0x0800fc00

int flash_write(const void *data, size_t data_len)
{
    if (data_len > FMC_PAGE_SIZE) {
    	rt_kprintf("data_len is too large\n");
    	return -1;
    }
    /* program flash */
    uint32_t address = FMC_WRITE_START_ADDR;
    uint16_t *data_word = (uint16_t *)data;

    fmc_unlock();
    /* erase */
    fmc_page_erase(address);
    /* write */
    for (uint32_t i = 0; i < data_len; i += sizeof(uint16_t) ) {
        fmc_halfword_program(address+i, *data_word);
	data_word++;
    
    }
    fmc_lock();

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

