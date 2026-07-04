#include "stm32f4xx_hal.h"
#include "stm_flash.h"

#define EE_SECTOR_ADDR 0x080E0000UL  // Sector 11
#define EE_SECTOR_NUM  FLASH_SECTOR_11
#define EE_SECTOR_SIZE  (128 * 1024)

static unsigned int stm32_flash_read(unsigned char *buf, unsigned int addr, unsigned int size)
{
    if (buf == NULL || size == 0)
        return 0;

    memcpy(buf, (const void *)addr, size);
    return size;
}

static unsigned int stm32_flash_write(unsigned char *buf, unsigned int offset, unsigned int size)
{
    unsigned int start = offset;
    unsigned int end   = offset + size;

    if (offset % 4 != 0) return 0;

    HAL_FLASH_Unlock();

    while (offset < end) {
        uint32_t data = *(uint32_t *)buf;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, offset, data) != HAL_OK)
            break;
        offset += 4;
        buf    += 4;
    }

    HAL_FLASH_Lock();
    return offset - start;
}

static unsigned int stm32_flash_erase(unsigned int offset, unsigned int size)
{
    FLASH_EraseInitTypeDef erase_init = {0};
    uint32_t sector_error = 0;

    erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector       = EE_SECTOR_NUM;
    erase_init.NbSectors    = 1;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();
    HAL_FLASHEx_Erase(&erase_init, &sector_error);
    HAL_FLASH_Lock();

    return (sector_error == 0xFFFFFFFF) ? EE_SECTOR_SIZE : 0;
}

static struct flash_ops stm32_flash = {
    "stm32_flash",
    stm32_flash_read,
    stm32_flash_write,
    stm32_flash_erase,
};

struct flash_ops *get_flash(void) { return &stm32_flash; }
