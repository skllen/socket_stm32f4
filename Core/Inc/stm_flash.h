#ifndef __STM_FLASH_H__
#define __STM_FLASH_H__


struct flash_ops{
    const char *name;
    unsigned int (*read)(unsigned char *buf, unsigned int offset, unsigned int size);
    unsigned int (*write)(unsigned char *buf, unsigned int offset, unsigned int size);
    unsigned int (*erase)(unsigned int offset, unsigned int size);
};


struct flash_ops *get_flash(void);

#endif
