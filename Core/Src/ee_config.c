/* ee_config.c */
#include "ee_config.h"
#include "stm_flash.h"
#include <stdio.h>
#include <string.h>
AppConfig_t g_config;

/* 每条记录 4 字节：低16bit = ID，高16bit = Value空白Flash = 0xFFFFFFFF */

// 找到下一个空槽，返回地址；扇区满返回 0
static uint32_t find_next_empty(void)
{
    uint32_t addr = EE_SECTOR_ADDR;
    while (addr < EE_SECTOR_ADDR + EE_SECTOR_SIZE) {
        if (*(uint32_t *)addr == 0xFFFFFFFF)
            return addr;
        addr += 4;
    }
    return 0;  // 满了
}

// 擦除扇区，把当前 g_config 重新写回去
static void EE_GarbageCollect(void)
{
    struct flash_ops *flash = get_flash();

    flash->erase(EE_SECTOR_ADDR, EE_SECTOR_SIZE);

    // 重写当前有效值
    uint32_t addr = EE_SECTOR_ADDR;
    uint32_t d;

    d = ((uint32_t)g_config.led_freq  << 16) | EE_ID_LED_FREQ;
    flash->write((unsigned char *)&d, addr, 4);
    addr += 4;

    d = ((uint32_t)g_config.led_state << 16) | EE_ID_LED_STATE;
    flash->write((unsigned char *)&d, addr, 4);
}

// 读变量：从头扫到尾，取最后一次匹配的值
uint16_t EE_ReadVar(uint16_t id, uint16_t default_val)
{
    uint32_t addr= EE_SECTOR_ADDR;
    uint16_t result = default_val;

    while (addr < EE_SECTOR_ADDR + EE_SECTOR_SIZE) {
        uint32_t raw = *(uint32_t *)addr;
        if (raw == 0xFFFFFFFF)
            break;  // 后面都是空的

        if ((raw & 0xFFFF) == id)
            result = (uint16_t)(raw >> 16);  // 持续覆盖，最终得到最新值

        addr += 4;
    }
    return result;
}

// 写变量：更新内存，追加写 Flash
void EE_WriteVar(uint16_t id, uint16_t value)
{
    struct flash_ops *flash = get_flash();

    // 先更新内存
    switch (id) {
        case EE_ID_LED_FREQ:  g_config.led_freq  = value; break;
        case EE_ID_LED_STATE: g_config.led_state = value; break;
        default: return;
    }

    // 找空槽
    uint32_t addr = find_next_empty();
    if (addr == 0) {
        // 扇区满，GC后重写（GC 里已经写了所有有效值）
        EE_GarbageCollect();
        return;
    }

    // 追加写
    uint32_t data = ((uint32_t)value << 16) | id;
    flash->write((unsigned char *)&data, addr, 4);
}

// 上电初始化：从 Flash 加载配置到内存
void EE_Init(void)
{
    g_config.led_freq  = EE_ReadVar(EE_ID_LED_FREQ,  DEFAULT_LED_FREQ);
    g_config.led_state = EE_ReadVar(EE_ID_LED_STATE, DEFAULT_LED_STATE);
}
//============================================================
// 测试1：Flash 底层驱动——擦、写、读
// ============================================================
static int test_flash_basic(void)
{
    struct flash_ops *flash = get_flash();
    uint8_t  wbuf[16];
    uint8_t  rbuf[16];
    int pass = 1;

    printf("\r\n[TEST1] Flash basic erase/write/read\r\n");

    // 1. 擦除 Sector 11
    printf("  Erasing sector 11... ");
    unsigned int erased = flash->erase(EE_SECTOR_ADDR, EE_SECTOR_SIZE);
    if (erased == 0) {
        printf("FAIL (erase returned 0)\r\n");
        return 0;
    }
    printf("OK\r\n");

    // 2. 擦完应全是0xFF
    printf("  Verifying erase (all 0xFF)... ");
    uint32_t *p = (uint32_t *)EE_SECTOR_ADDR;
    int blank_ok = 1;
    for (int i = 0; i < 16; i++) {       // 只检前16 个word
        if (p[i] != 0xFFFFFFFF) {
            blank_ok = 0;
            printf("FAIL at offset %d, val=0x%08X\r\n", i * 4, p[i]);
            break;
        }
    }
    if (blank_ok) printf("OK\r\n");
    else return 0;

    // 3. 写入测试数据
    printf("  Writing test pattern... ");
    for (int i = 0; i < 16; i++) wbuf[i] = 0xA0 + i;
    unsigned int written = flash->write(wbuf, EE_SECTOR_ADDR, 16);
    if (written != 16) {
        printf("FAIL (wrote %d bytes)\r\n", written);
        return 0;
    }
    printf("OK\r\n");

    // 4. 读回校验
    printf("  Reading back and verify... ");
    flash->read(rbuf, EE_SECTOR_ADDR, 16);
    if (memcmp(wbuf, rbuf, 16) != 0) {
        printf("FAIL\r\n  Written: ");
        for (int i = 0; i < 16; i++) printf("%02X ", wbuf[i]);
        printf("\r\n  Read:");
        for (int i = 0; i < 16; i++) printf("%02X ", rbuf[i]);
        printf("\r\n");
        return 0;
    }
    printf("OK\r\n");

    printf("[TEST1] PASS\r\n");
    return 1;
}

// ============================================================
// 测试2：EE 写、读
// ============================================================
static int test_ee_write_read(void)
{
    printf("\r\n[TEST2] EE write/read\r\n");

    // 先擦干净，重新初始化
    struct flash_ops *flash = get_flash();
    flash->erase(EE_SECTOR_ADDR, EE_SECTOR_SIZE);
    EE_Init();  // 应加载默认值

    printf("  After init: led_freq=%d (expect %d)led_state=%d (expect %d)\r\n",
             g_config.led_freq,  DEFAULT_LED_FREQ,
             g_config.led_state, DEFAULT_LED_STATE);

    if (g_config.led_freq!= DEFAULT_LED_FREQ ||
        g_config.led_state != DEFAULT_LED_STATE) {
        printf("[TEST2] FAIL (default value mismatch)\r\n");
        return 0;
    }

    // 写入新值
    EE_WriteVar(EE_ID_LED_FREQ,  300);
    EE_WriteVar(EE_ID_LED_STATE, 1);

    printf("  After write: led_freq=%d (expect 300)  led_state=%d (expect 1)\r\n",
             g_config.led_freq, g_config.led_state);

    if (g_config.led_freq != 300 || g_config.led_state != 1) {
        printf("[TEST2] FAIL (write value mismatch)\r\n");
        return 0;
    }

    // 重新 EE_Init 模拟断电重启，看能不能读回来
    EE_Init();
    printf("  After re-init: led_freq=%d (expect 300)  led_state=%d (expect 1)\r\n",
             g_config.led_freq, g_config.led_state);

    if (g_config.led_freq != 300 || g_config.led_state != 1) {
        printf("[TEST2] FAIL (value lost after re-init)\r\n");
        return 0;
    }

    printf("[TEST2] PASS\r\n");
    return 1;
}

// ============================================================
// 测试3：多次写入，验证总是读到最新值
// ============================================================
static int test_ee_latest_value(void)
{
    printf("\r\n[TEST3] EE always returns latest value\r\n");

    struct flash_ops *flash = get_flash();
    flash->erase(EE_SECTOR_ADDR, EE_SECTOR_SIZE);

    uint16_t vals[] = {100, 200, 300, 400, 500};
    for (int i = 0; i < 5; i++) {
        EE_WriteVar(EE_ID_LED_FREQ, vals[i]);
    }

    uint16_t result = EE_ReadVar(EE_ID_LED_FREQ, DEFAULT_LED_FREQ);
    printf("  After 5 writes, read=%d (expect 500)\r\n", result);

    if (result != 500) {
        printf("[TEST3] FAIL\r\n");
        return 0;
    }

    printf("[TEST3] PASS\r\n");
    return 1;
}

// ============================================================
// 测试4：写满触发 GC，GC 后值是否正确
// ============================================================
static int test_ee_gc(void)
{
    printf("\r\n[TEST4] EE garbage collect\r\n");

    struct flash_ops *flash = get_flash();
    flash->erase(EE_SECTOR_ADDR, EE_SECTOR_SIZE);

    // Sector 11 共 128KB/4 = 32768 个槽
    // 先写到只剩 1 个槽
    int total_slots = EE_SECTOR_SIZE / 4;
    printf("  Filling %d slots...\r\n", total_slots - 1);

    for (int i = 0; i < total_slots - 1; i++) {
        EE_WriteVar(EE_ID_LED_FREQ, (uint16_t)(i % 1000));
    }

    // 此时再写一次，应触发 GC
    printf("  Triggering GC with one more write...\r\n");
    EE_WriteVar(EE_ID_LED_FREQ, 888);

    // GC 后读回
    uint16_t result = EE_ReadVar(EE_ID_LED_FREQ, 0);
    printf("  After GC, led_freq=%d (expect 888)\r\n", result);

    if (result != 888) {
        printf("[TEST4] FAIL\r\n");
        return 0;
    }

    printf("[TEST4] PASS\r\n");
    return 1;
}

// ============================================================
// 入口：跑所有测试
// ============================================================
void flash_test_run(void)
{
    int pass = 0, total = 4;

    printf("\r\n========== Flash Storage Test ==========\r\n");

    pass += test_flash_basic();
    pass += test_ee_write_read();
    pass += test_ee_latest_value();
    pass += test_ee_gc();

    printf("\r\n========== Result: %d/%d PASS ==========\r\n", pass, total);
}
