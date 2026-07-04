/* ee_config.h */
#ifndef EE_CONFIG_H
#define EE_CONFIG_H

#include <stdint.h>

// Sector 11: 0x080E0000, 128KB
#define EE_SECTOR_ADDR  0x080E0000UL
#define EE_SECTOR_SIZE  (128 * 1024)

// 变量ID
#define EE_ID_LED_FREQ   0x0001
#define EE_ID_LED_STATE  0x0002

// 默认值
#define DEFAULT_LED_FREQ   500
#define DEFAULT_LED_STATE  0

typedef struct {
    uint16_t led_freq;   // 闪烁周期 ms
    uint16_t led_state;  // 0=关1=开
} AppConfig_t;

extern AppConfig_t g_config;

void 		 EE_Init(void);
void     EE_WriteVar(uint16_t id, uint16_t value);
uint16_t EE_ReadVar(uint16_t id, uint16_t default_val);
void flash_test_run(void);

#endif
