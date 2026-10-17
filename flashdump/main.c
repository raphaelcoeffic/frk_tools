/*
 * Flash dumper for STM32F1 receivers running the FrSky S.Port bootloader.
 *
 * Upload it like any firmware (see Makefile / PROTOCOL.md). It then sends
 * the flash range [DUMP_START, DUMP_START + DUMP_LENGTH) over the S.Port
 * line, forever. Reading flash from code running on the chip works even with
 * readout protection enabled.
 *
 * Each round is framed as:
 *
 *   "FDMP" | start (u32 LE) | length (u32 LE) | data | CRC-32 of data (u32 LE)
 *
 * The CRC is the usual zlib/IEEE one, so extract_dump.py can find and check
 * a round in a raw capture.
 *
 * The UART, its pins and the transmit-enable pin are taken from the
 * bootloader's config page, so this works on any board using the same
 * bootloader. No CMSIS or vendor headers: registers are accessed directly.
 */

#include <stdint.h>

#ifndef DUMP_START
#define DUMP_START   0x08000000u
#endif

#ifndef DUMP_LENGTH
#define DUMP_LENGTH  0x2000u
#endif

#define BAUDRATE     57600u
#define HSI_FREQ     8000000u

#define REG(addr)    (*(volatile uint32_t *)(addr))

/* RCC */
#define RCC_BASE     0x40021000u
#define RCC_CR       REG(RCC_BASE + 0x00)
#define RCC_CFGR     REG(RCC_BASE + 0x04)
#define RCC_APB2ENR  REG(RCC_BASE + 0x18)
#define RCC_APB1ENR  REG(RCC_BASE + 0x1C)

#define RCC_CR_HSION       (1u << 0)
#define RCC_CR_HSIRDY      (1u << 1)
#define RCC_CFGR_SWS_MASK  (3u << 2)

/* GPIO (port base + offset) */
#define GPIO_PORTA   0x40010800u
#define GPIO_PORTB   0x40010C00u
#define GPIO_CRL     0x00
#define GPIO_CRH     0x04
#define GPIO_BSRR    0x10
#define GPIO_BRR     0x14

/* CNF/MODE nibbles */
#define GPIO_IN_FLOATING  0x4u
#define GPIO_OUT_PP       0x3u   /* push-pull, 50 MHz */
#define GPIO_AF_PP        0xBu   /* alternate function push-pull, 50 MHz */

/* USART (base + offset) */
#define USART1_BASE  0x40013800u
#define USART2_BASE  0x40004400u
#define USART3_BASE  0x40004800u
#define USART_SR     0x00
#define USART_DR     0x04
#define USART_BRR    0x08
#define USART_CR1    0x0C
#define USART_CR2    0x10
#define USART_CR3    0x14

#define USART_SR_TC     (1u << 6)
#define USART_SR_TXE    (1u << 7)
#define USART_CR1_TE    (1u << 3)
#define USART_CR1_UE    (1u << 13)

/* Independent watchdog (refreshed in case the option bytes enable it) */
#define IWDG_KR      REG(0x40003000u)

/*
 * Bootloader config page (see PROTOCOL.md)
 */
#define CONFIG_BASE  0x08001C00u
#define CONFIG_MAGIC 0x9ABCDEF0u

struct pin {
    uint32_t port;
    uint16_t pin;
    uint16_t flags;
};

struct config {
    uint32_t   app_addr;      /* 0x00 */
    uint32_t   app_addr_inv;  /* 0x04 */
    uint32_t   unused;        /* 0x08 */
    uint32_t   magic;         /* 0x0C */
    uint32_t   unknown;       /* 0x10 */
    struct pin led1;          /* 0x14 */
    struct pin led2;          /* 0x1C */
    struct pin tx_enable;     /* 0x24 */
    struct pin tx;            /* 0x2C */
    struct pin rx;            /* 0x34 */
    uint32_t   usart;         /* 0x3C */
};

/* X4R values, used if the config page doesn't look right */
static const struct config x4r_config = {
    .tx_enable = { GPIO_PORTA,  2, 0 },
    .tx        = { GPIO_PORTB, 10, 0 },
    .rx        = { GPIO_PORTB, 11, 0 },
    .usart     = USART3_BASE,
};

static void gpio_enable_clock(uint32_t port)
{
    /* IOPAEN is bit 2, ports are 0x400 apart */
    RCC_APB2ENR |= 1u << (2 + (port - GPIO_PORTA) / 0x400);
}

static void gpio_mode(const struct pin *p, uint32_t mode)
{
    uint32_t reg   = p->port + (p->pin < 8 ? GPIO_CRL : GPIO_CRH);
    uint32_t shift = (p->pin & 7) * 4;

    gpio_enable_clock(p->port);
    REG(reg) = (REG(reg) & ~(0xFu << shift)) | (mode << shift);
}

static void gpio_set(const struct pin *p, int on)
{
    REG(p->port + (on ? GPIO_BSRR : GPIO_BRR)) = 1u << p->pin;
}

/*
 * The bootloader may have left the PLL running: go back to the 8 MHz HSI
 * with all bus prescalers at 1, so the baud rate is known.
 */
static void clock_init(void)
{
    RCC_CR |= RCC_CR_HSION;
    while (!(RCC_CR & RCC_CR_HSIRDY))
        ;

    RCC_CFGR = 0;
    while (RCC_CFGR & RCC_CFGR_SWS_MASK)
        ;
}

static void usart_init(uint32_t usart)
{
    if (usart == USART1_BASE)
        RCC_APB2ENR |= 1u << 14;
    else if (usart == USART2_BASE)
        RCC_APB1ENR |= 1u << 17;
    else
        RCC_APB1ENR |= 1u << 18;

    REG(usart + USART_CR1) = 0;
    REG(usart + USART_CR2) = 0;
    REG(usart + USART_CR3) = 0;
    REG(usart + USART_BRR) = (HSI_FREQ + BAUDRATE / 2) / BAUDRATE;
    REG(usart + USART_CR1) = USART_CR1_UE | USART_CR1_TE;
}

static void usart_putc(uint32_t usart, uint8_t c)
{
    while (!(REG(usart + USART_SR) & USART_SR_TXE))
        ;
    REG(usart + USART_DR) = c;
    IWDG_KR = 0xAAAA;
}

static void usart_put_u32(uint32_t usart, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        usart_putc(usart, (uint8_t)(v >> (8 * i)));
}

static uint32_t crc32_update(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    return crc;
}

static void delay(uint32_t n)
{
    while (n--) {
        IWDG_KR = 0xAAAA;
        __asm__ volatile ("nop");
    }
}

int main(void)
{
    const struct config *cfg = (const struct config *)CONFIG_BASE;
    int has_leds = 1;

    if (cfg->magic != CONFIG_MAGIC || (cfg->app_addr ^ cfg->app_addr_inv) != 0xFFFFFFFFu) {
        cfg = &x4r_config;
        has_leds = 0;
    }

    clock_init();

    gpio_set(&cfg->tx_enable, 1);
    gpio_mode(&cfg->tx_enable, GPIO_OUT_PP);
    gpio_mode(&cfg->tx, GPIO_AF_PP);
    gpio_mode(&cfg->rx, GPIO_IN_FLOATING);

    if (has_leds && (cfg->led1.flags & 1))
        gpio_mode(&cfg->led1, GPIO_OUT_PP);
    else
        has_leds = 0;

    usart_init(cfg->usart);

    for (int led = 0;; led ^= 1) {
        const uint8_t *p   = (const uint8_t *)DUMP_START;
        const uint8_t *end = p + DUMP_LENGTH;
        uint32_t crc = 0xFFFFFFFFu;

        if (has_leds)
            gpio_set(&cfg->led1, led);

        usart_putc(cfg->usart, 'F');
        usart_putc(cfg->usart, 'D');
        usart_putc(cfg->usart, 'M');
        usart_putc(cfg->usart, 'P');
        usart_put_u32(cfg->usart, DUMP_START);
        usart_put_u32(cfg->usart, DUMP_LENGTH);

        while (p < end) {
            usart_putc(cfg->usart, *p);
            crc = crc32_update(crc, *p++);
        }

        usart_put_u32(cfg->usart, ~crc);

        /* quiet gap between rounds (~0.5 s at 8 MHz) */
        delay(1000000);
    }
}

/*
 * Startup
 */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    main();
    for (;;)
        ;
}

static void Default_Handler(void)
{
    for (;;)
        ;
}

/*
 * Only the core exceptions: the bootloader jumps here with interrupts
 * disabled and nothing enables them.
 */
__attribute__((section(".isr_vector"), used))
static void (* const vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Default_Handler,  /* NMI */
    Default_Handler,  /* HardFault */
    Default_Handler,  /* MemManage */
    Default_Handler,  /* BusFault */
    Default_Handler,  /* UsageFault */
    0, 0, 0, 0,
    Default_Handler,  /* SVCall */
    Default_Handler,  /* DebugMon */
    0,
    Default_Handler,  /* PendSV */
    Default_Handler,  /* SysTick */
};
