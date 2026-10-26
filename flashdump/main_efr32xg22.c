/*
 * Flash dumper for Silicon Labs EFR32xG22 devices running FrSky's S.Port
 * bootloader with the application at 0x00008000, e.g. the Archer Plus
 * SR10+ / R10+ receivers.
 *
 * Upload it like any firmware (see Makefile / PROTOCOL.md). It then sends
 * the flash range [DUMP_START, DUMP_START + DUMP_LENGTH) over the S.Port
 * line, forever. By default (DUMP_LENGTH 0) that is the whole flash, its
 * size read from DEVINFO: the bootloader, whatever is left of the
 * application, and any data pages.
 *
 * Each round is framed like the STM32 version's, so extract_dump.py and
 * pico/ read it unchanged:
 *
 *   "FDMP" | start (u32 LE) | length (u32 LE) | data | CRC-32 of data (u32 LE)
 *
 * The pins are build options, as port * 16 + pin (PB04 = 0x14), -1 for
 * "none". The defaults come from the SR10+ 1.0.10 application: USART0 TX on
 * PB00 (RX on PB01), and PB04 held low while sending, high otherwise. The
 * USART doesn't invert anything, so the line driver outside the chip does.
 *
 * FrSky's bootloaders leave the chip in unusual states (the XJT-Lite's
 * jumps to the application on the process stack), so nothing is assumed:
 * stack, stack limits, MPU, interrupts, DMA, clocks, locks, the USART and
 * the pin routes are all set from scratch, and the watchdog is fed if it
 * runs. The system clock is the HFRCO in its 19 MHz band, with the factory
 * calibration from DEVINFO, whatever the bootloader used.
 *
 * RX isn't touched: the dumper only sends.
 *
 * Nothing is written to flash here.
 * No CMSIS or vendor headers: registers are accessed directly. Offsets are
 * from the EFR32BG22 headers of the Gecko SDK 4.4.
 */

#include <stdint.h>

#ifndef DUMP_START
#define DUMP_START   0x00000000u
#endif

/* 0: the whole flash, from DEVINFO */
#ifndef DUMP_LENGTH
#define DUMP_LENGTH  0u
#endif

/* used when DEVINFO gives no sensible flash size: the smallest xG22 */
#define FLASH_SIZE_FALLBACK  (352u * 1024u)

#define BAUDRATE     57600u
#define HFRCO_FREQ   19000000u   /* the 19 MHz band */

/* port * 16 + pin */
#ifndef PIN_TX
#define PIN_TX         0x10      /* PB00 */
#endif

#ifndef PIN_TX_ENABLE
#define PIN_TX_ENABLE  0x14      /* PB04, low = transmit */
#endif

#define PORT(p)      ((uint32_t)(p) >> 4)
#define PIN(p)       ((uint32_t)(p) & 15u)

#define REG(addr)    (*(volatile uint32_t *)(addr))

/*
 * Series 2 peripherals have SET, CLR and TGL aliases of all their
 * registers at +0x1000, +0x2000 and +0x3000.
 */
#define SET          0x1000u
#define CLR          0x2000u

/* CMU */
#define CMU_BASE           0x40008000u
#define CMU_LOCK           REG(CMU_BASE + 0x010)
#define CMU_CLKEN0         REG(CMU_BASE + 0x064)
#define CMU_SYSCLKCTRL     REG(CMU_BASE + 0x070)

#define CMU_LOCK_UNLOCK          0x93F7u
#define CMU_CLKEN0_LDMA          (1u << 0)
#define CMU_CLKEN0_LDMAXBAR      (1u << 1)
#define CMU_CLKEN0_USART0        (1u << 8)
#define CMU_CLKEN0_WDOG0         (1u << 13)
#define CMU_CLKEN0_DPLL0         (1u << 17)
#define CMU_CLKEN0_HFRCO0        (1u << 18)
#define CMU_CLKEN0_GPIO          (1u << 26)
#define CMU_SYSCLKCTRL_CLKSEL_MASK      0x7u
#define CMU_SYSCLKCTRL_CLKSEL_FSRCO     1u
#define CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL 2u
#define CMU_SYSCLKCTRL_PCLKPRESC_MASK   0x400u    /* 0: HCLK / 1 */
#define CMU_SYSCLKCTRL_HCLKPRESC_MASK   0xF000u   /* 0: SYSCLK / 1 */

/* HFRCO0 */
#define HFRCO0_BASE        0x40010000u
#define HFRCO0_CAL         REG(HFRCO0_BASE + 0x008)
#define HFRCO0_STATUS      REG(HFRCO0_BASE + 0x00C)
#define HFRCO0_LOCK        REG(HFRCO0_BASE + 0x01C)

#define HFRCO_LOCK_UNLOCK        0x8195u
#define HFRCO_STATUS_FREQBSY     (1u << 1)
#define HFRCO_STATUS_SYNCBUSY    (1u << 2)
#define HFRCO_CAL_CLKDIV_MASK    (3u << 24)

/* DPLL0: it retunes the HFRCO when enabled */
#define DPLL0_BASE         0x4001C000u
#define DPLL0_EN           REG(DPLL0_BASE + 0x004)
#define DPLL0_STATUS       REG(DPLL0_BASE + 0x018)
#define DPLL0_LOCK         REG(DPLL0_BASE + 0x024)

#define DPLL_LOCK_UNLOCK         0x7102u
#define DPLL_STATUS_RDY          (1u << 0)
#define DPLL_STATUS_ENS          (1u << 1)

/* DEVINFO */
#define DEVINFO_BASE       0x0FE08000u
#define DEVINFO_MSIZE      REG(DEVINFO_BASE + 0x00C)   /* bits 0..15: flash KB */
#define DEVINFO_HFRCODPLLCAL(n) REG(DEVINFO_BASE + 0x058 + 4u * (n))
#define DEVINFO_HFRCODPLLCAL_19M  8

/* LDMA */
#define LDMA_BASE          0x40040000u
#define LDMA_EN            REG(LDMA_BASE + 0x004)
#define LDMA_CHDIS         REG(LDMA_BASE + 0x028)
#define LDMA_CHBUSY        REG(LDMA_BASE + 0x030)
#define LDMA_IEN           REG(LDMA_BASE + 0x054)

/* GPIO */
#define GPIO_BASE          0x4003C000u
#define GPIO_MODEL(port)   REG(GPIO_BASE + 0x30u * (port) + 0x004)
#define GPIO_MODEH(port)   REG(GPIO_BASE + 0x30u * (port) + 0x00C)
#define GPIO_DOUT(port)    (GPIO_BASE + 0x30u * (port) + 0x010)
#define GPIO_LOCK          REG(GPIO_BASE + 0x300)
#define GPIO_EUART0_ROUTEEN   REG(GPIO_BASE + 0x4C0)
#define GPIO_USART0_ROUTEEN   REG(GPIO_BASE + 0x61C)
#define GPIO_USART0_TXROUTE   REG(GPIO_BASE + 0x634)
#define GPIO_USART1_ROUTEEN   REG(GPIO_BASE + 0x63C)

#define GPIO_LOCK_UNLOCK         0xA534u
#define GPIO_MODE_INPUT          1u
#define GPIO_MODE_PUSHPULL       4u
#define GPIO_ROUTEEN_TXPEN       (1u << 4)

/* USART0 */
#define USART0_BASE        0x4005C000u
#define USART_EN           REG(USART0_BASE + 0x004)
#define USART_CTRL         REG(USART0_BASE + 0x008)
#define USART_FRAME        REG(USART0_BASE + 0x00C)
#define USART_TRIGCTRL     REG(USART0_BASE + 0x010)
#define USART_CMD          REG(USART0_BASE + 0x014)
#define USART_STATUS       REG(USART0_BASE + 0x018)
#define USART_CLKDIV       REG(USART0_BASE + 0x01C)
#define USART_TXDATA       REG(USART0_BASE + 0x03C)
#define USART_IF           REG(USART0_BASE + 0x048)
#define USART_IEN          REG(USART0_BASE + 0x04C)
#define USART_IRCTRL       REG(USART0_BASE + 0x050)
#define USART_I2SCTRL      REG(USART0_BASE + 0x054)
#define USART_TIMING       REG(USART0_BASE + 0x058)
#define USART_CTRLX        REG(USART0_BASE + 0x05C)
#define USART_TIMECMP(n)   REG(USART0_BASE + 0x060 + 4u * (n))

#define USART_EN_EN              (1u << 0)
#define USART_CMD_RXDIS          (1u << 1)
#define USART_CMD_TXEN           (1u << 2)
#define USART_CMD_TXDIS          (1u << 3)
#define USART_CMD_MASTERDIS      (1u << 5)
#define USART_CMD_RXBLOCKDIS     (1u << 7)
#define USART_CMD_TXTRIDIS       (1u << 9)
#define USART_CMD_CLEARTX        (1u << 10)
#define USART_CMD_CLEARRX        (1u << 11)
#define USART_STATUS_TXC         (1u << 5)
#define USART_STATUS_TXBL        (1u << 6)
#define USART_FRAME_8N1          ((5u << 0) | (0u << 8) | (1u << 12))
#define USART_CLKDIV_DIV_MASK    0x7FFFF8u

/*
 * Watchdog: FrSky's bootloader may have started it, and it may be locked,
 * so just feed it as we go if it runs.
 */
#define WDOG0_BASE         0x4A018000u
#define WDOG0_EN           REG(WDOG0_BASE + 0x004)
#define WDOG0_CMD          REG(WDOG0_BASE + 0x00C)

#define WDOG_EN_EN               (1u << 0)
#define WDOG_CMD_CLEAR           (1u << 0)

/* Core */
#define SYST_CSR           REG(0xE000E010u)
#define NVIC_ICER(n)       REG(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n)       REG(0xE000E280u + 4u * (n))
#define SCB_ICSR           REG(0xE000ED04u)
#define SCB_VTOR           REG(0xE000ED08u)
#define MPU_CTRL           REG(0xE000ED94u)

#define SCB_ICSR_PENDSTCLR       (1u << 25)
#define SCB_ICSR_PENDSVCLR       (1u << 27)

static int wdog_running;

static void wdog_feed(void)
{
    if (wdog_running)
        WDOG0_CMD = WDOG_CMD_CLEAR;
}

static void unlock(void)
{
    CMU_LOCK = CMU_LOCK_UNLOCK;
    CMU_CLKEN0 |= CMU_CLKEN0_LDMA | CMU_CLKEN0_LDMAXBAR | CMU_CLKEN0_USART0
                | CMU_CLKEN0_WDOG0 | CMU_CLKEN0_DPLL0 | CMU_CLKEN0_HFRCO0
                | CMU_CLKEN0_GPIO;

    HFRCO0_LOCK = HFRCO_LOCK_UNLOCK;
    DPLL0_LOCK  = DPLL_LOCK_UNLOCK;
    GPIO_LOCK   = GPIO_LOCK_UNLOCK;

    wdog_running = (WDOG0_EN & WDOG_EN_EN) != 0;
    wdog_feed();
}

/* stop any DMA transfer the bootloader left running, e.g. into the USART */
static void dma_stop(void)
{
    LDMA_IEN   = 0;
    LDMA_CHDIS = 0xFFu;
    while (LDMA_CHBUSY & 0xFFu)
        wdog_feed();
    LDMA_EN = 0;
}

static void clock_init(void)
{
    uint32_t cal = DEVINFO_HFRCODPLLCAL(DEVINFO_HFRCODPLLCAL_19M);

    /* run from the FSRCO while the HFRCO is retuned */
    CMU_SYSCLKCTRL = (CMU_SYSCLKCTRL & ~CMU_SYSCLKCTRL_CLKSEL_MASK)
                   | CMU_SYSCLKCTRL_CLKSEL_FSRCO;

    /* the DPLL would retune the HFRCO behind our back */
    if (DPLL0_EN) {
        DPLL0_EN = 0;
        while (DPLL0_STATUS & (DPLL_STATUS_ENS | DPLL_STATUS_RDY))
            wdog_feed();
    }

    /* CAL updates are deferred while the HFRCO is busy */
    while (HFRCO0_STATUS & (HFRCO_STATUS_SYNCBUSY | HFRCO_STATUS_FREQBSY))
        wdog_feed();

    /* the 19 MHz band, undivided; keep the current one if DEVINFO is blank */
    if (cal != 0 && cal != 0xFFFFFFFFu)
        HFRCO0_CAL = cal & ~HFRCO_CAL_CLKDIV_MASK;

    while (HFRCO0_STATUS & (HFRCO_STATUS_SYNCBUSY | HFRCO_STATUS_FREQBSY))
        wdog_feed();

    /* SYSCLK = HCLK = PCLK = HFRCO; the USART runs from PCLK */
    CMU_SYSCLKCTRL = (CMU_SYSCLKCTRL & ~(CMU_SYSCLKCTRL_CLKSEL_MASK
                                         | CMU_SYSCLKCTRL_PCLKPRESC_MASK
                                         | CMU_SYSCLKCTRL_HCLKPRESC_MASK))
                   | CMU_SYSCLKCTRL_CLKSEL_HFRCODPLL;
}

static void pin_mode(int p, uint32_t mode)
{
    uint32_t shift = 4u * (PIN(p) & 7u);
    volatile uint32_t *reg = PIN(p) < 8 ? &GPIO_MODEL(PORT(p))
                                        : &GPIO_MODEH(PORT(p));

    *reg = (*reg & ~(0xFu << shift)) | (mode << shift);
}

static void pin_set(int p, int high)
{
    REG(GPIO_DOUT(PORT(p)) + (high ? SET : CLR)) = 1u << PIN(p);
}

static void tx_enable(int on)
{
    if (PIN_TX_ENABLE >= 0)
        pin_set(PIN_TX_ENABLE, !on);
}

static void pins_init(void)
{
    /* nothing else on the UART pins */
    GPIO_EUART0_ROUTEEN = 0;
    GPIO_USART0_ROUTEEN = 0;
    GPIO_USART1_ROUTEEN = 0;

    /* transmit-enable: output, high (released) to start with */
    if (PIN_TX_ENABLE >= 0) {
        tx_enable(0);
        pin_mode(PIN_TX_ENABLE, GPIO_MODE_PUSHPULL);
    }

    /* TX idles high, as the USART would */
    pin_set(PIN_TX, 1);
    pin_mode(PIN_TX, GPIO_MODE_PUSHPULL);

    GPIO_USART0_TXROUTE = PORT(PIN_TX) | (PIN(PIN_TX) << 16);
    GPIO_USART0_ROUTEEN = GPIO_ROUTEEN_TXPEN;
}

static void uart_init(void)
{
    /*
     * 16x oversampling: CLKDIV = 256 * (f / (16 * baud) - 1), whose 3 low
     * bits are always 0. 19 MHz: 5024 -> 57576 baud (-0.04 %).
     */
    uint32_t div = (32u * HFRCO_FREQ / BAUDRATE + 1) / 2 - 256u;

    /* as em_usart's USART_Reset() */
    USART_EN  = USART_EN_EN;
    USART_CMD = USART_CMD_RXDIS | USART_CMD_TXDIS | USART_CMD_MASTERDIS
              | USART_CMD_RXBLOCKDIS | USART_CMD_TXTRIDIS
              | USART_CMD_CLEARTX | USART_CMD_CLEARRX;
    USART_IEN      = 0;
    USART_CTRL     = 0;      /* asynchronous, 16x oversampling, no inversion */
    USART_FRAME    = USART_FRAME_8N1;
    USART_TRIGCTRL = 0;
    USART_IRCTRL   = 0;
    USART_I2SCTRL  = 0;
    USART_TIMING   = 0;
    USART_CTRLX    = 0;
    for (int i = 0; i < 3; i++)
        USART_TIMECMP(i) = 0;
    REG(USART0_BASE + CLR + 0x048) = 0xFFFFFFFFu;   /* IF */
    USART_CLKDIV = ((div + 4u) & ~7u) & USART_CLKDIV_DIV_MASK;

    USART_CMD = USART_CMD_TXEN;
}

static void uart_putc(uint8_t c)
{
    while (!(USART_STATUS & USART_STATUS_TXBL))
        ;
    USART_TXDATA = c;
    wdog_feed();
}

static void uart_put_u32(uint32_t v)
{
    for (int i = 0; i < 4; i++)
        uart_putc((uint8_t)(v >> (8 * i)));
}

static void uart_flush(void)
{
    while (!(USART_STATUS & USART_STATUS_TXC))
        ;
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
        wdog_feed();
        __asm__ volatile ("nop");
    }
}

static uint32_t dump_length(void)
{
    uint32_t kb;

    if (DUMP_LENGTH)
        return DUMP_LENGTH;

    kb = DEVINFO_MSIZE & 0xFFFFu;
    if (kb == 0 || kb > 2048)
        return FLASH_SIZE_FALLBACK - DUMP_START;
    return kb * 1024u - DUMP_START;
}

int main(void)
{
    uint32_t length;

    unlock();
    dma_stop();
    clock_init();
    pins_init();
    uart_init();

    length = dump_length();

    for (;;) {
        const uint8_t *p   = (const uint8_t *)DUMP_START;
        const uint8_t *end = p + length;
        uint32_t crc = 0xFFFFFFFFu;

        tx_enable(1);

        uart_putc('F');
        uart_putc('D');
        uart_putc('M');
        uart_putc('P');
        uart_put_u32(DUMP_START);
        uart_put_u32(length);

        while (p < end) {
            uart_putc(*p);
            crc = crc32_update(crc, *p++);
        }

        uart_put_u32(~crc);
        uart_flush();

        /* release the line for a quiet gap between rounds (~0.5 s) */
        tx_enable(0);
        delay(2000000);
    }
}

/*
 * Startup
 */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
static void (* const vectors[16])(void);

static void __attribute__((used)) reset_c(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    /*
     * The bootloader may have left interrupts, the RTOS tick or the MPU
     * running: none of them may reach our vector table or our memory.
     */
    SYST_CSR = 0;
    for (int i = 0; i < 16; i++) {
        NVIC_ICER(i) = 0xFFFFFFFFu;
        NVIC_ICPR(i) = 0xFFFFFFFFu;
    }
    SCB_ICSR = SCB_ICSR_PENDSTCLR | SCB_ICSR_PENDSVCLR;
    MPU_CTRL = 0;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    SCB_VTOR = (uint32_t)vectors;

    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    main();
    for (;;)
        ;
}

/*
 * Before anything touches the stack: interrupts off, no stack limits, the
 * main stack at the top of our RAM, privileged thread mode on that stack
 * (the XJT-Lite's bootloader jumps on the process stack).
 */
__attribute__((naked)) void Reset_Handler(void)
{
    __asm__ volatile (
        "cpsid i\n\t"
        "movs  r0, #0\n\t"
        "msr   msplim, r0\n\t"
        "msr   psplim, r0\n\t"
        "ldr   r1, =_estack\n\t"
        "msr   msp, r1\n\t"
        "msr   control, r0\n\t"
        "isb\n\t"
        "b     reset_c\n\t"
    );
}

static void Default_Handler(void)
{
    for (;;)
        ;
}

/* Only the core exceptions: interrupts stay disabled. */
__attribute__((section(".isr_vector"), used))
static void (* const vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Default_Handler,  /* NMI */
    Default_Handler,  /* HardFault */
    Default_Handler,  /* MemManage */
    Default_Handler,  /* BusFault */
    Default_Handler,  /* UsageFault */
    Default_Handler,  /* SecureFault */
    0, 0, 0,
    Default_Handler,  /* SVCall */
    Default_Handler,  /* DebugMon */
    0,
    Default_Handler,  /* PendSV */
    Default_Handler,  /* SysTick */
};
