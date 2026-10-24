/*
 * Flash dumper for TI CC26x0 (CC2650) devices running FrSky's CC26xx
 * S.Port bootloader, e.g. the XJT-Lite, with the application at 0x00008000.
 *
 * Upload it like any firmware (see Makefile / PROTOCOL.md). It then sends
 * the flash range [DUMP_START, DUMP_START + DUMP_LENGTH) over the S.Port
 * line, forever. By default that is the whole 128 KB flash: the bootloader,
 * whatever is left of the application, and the CCFG in the last page. Code
 * running from flash can read all of it, even with the debug port locked.
 *
 * Each round is framed like the STM32 version's, so extract_dump.py and
 * pico/ read it unchanged:
 *
 *   "FDMP" | start (u32 LE) | length (u32 LE) | data | CRC-32 of data (u32 LE)
 *
 * Unlike the STM32 bootloader's, these bootloaders have no config page to
 * read the pins from: they're compiled into TI-RTOS board tables. So the
 * pins are build options, -1 for "none":
 *
 *   - XJT-Lite (default): UART0 TX on DIO29, DIO24 low while sending. DIO29
 *     feeds an inverting 3-state buffer (LVC2G240-like) whose output goes to
 *     the S.Port pin through 68 ohms. Its output enable has a 10k pull-up, so
 *     the line is only driven while DIO24 is low. That is also what the
 *     bootloader does (with RXE off while sending).
 *   - RX6R: UART0 TX on DIO25, DIO24 low while transmitting (from its
 *     bootloader).
 *
 * DIO_TX2 sends the same output on a second pin, e.g. DIO25 on the XJT-Lite,
 * whose "TX" test pad it reaches (inverted).
 *
 * RX isn't touched: the dumper only sends.
 *
 * Nothing is written to flash here, and the CCFG is only read.
 * No CMSIS or vendor headers: registers are accessed directly.
 */

#include <stdint.h>

#ifndef DUMP_START
#define DUMP_START   0x00000000u
#endif

#ifndef DUMP_LENGTH
#define DUMP_LENGTH  0x20000u
#endif

#define BAUDRATE     57600u
/* The system clock is 48 MHz, from RCOSC_HF or the doubled 24 MHz XOSC */
#define CPU_FREQ     48000000u

#ifndef DIO_TX
#define DIO_TX         29
#endif

/* optional second pin with the same output, -1 for none */
#ifndef DIO_TX2
#define DIO_TX2        -1
#endif

#ifndef DIO_TX_ENABLE
#define DIO_TX_ENABLE  24   /* low = transmit */
#endif

#define REG(addr)    (*(volatile uint32_t *)(addr))

/* PRCM */
#define PRCM_BASE          0x40082000u
#define PRCM_CLKLOADCTL    REG(PRCM_BASE + 0x028)
#define PRCM_GPIOCLKGR     REG(PRCM_BASE + 0x048)
#define PRCM_UARTCLKGR     REG(PRCM_BASE + 0x06C)
#define PRCM_PDCTL0SERIAL  REG(PRCM_BASE + 0x134)
#define PRCM_PDCTL0PERIPH  REG(PRCM_BASE + 0x138)
#define PRCM_PDSTAT0       REG(PRCM_BASE + 0x140)

#define PRCM_CLKLOADCTL_LOAD       (1u << 0)
#define PRCM_CLKLOADCTL_LOAD_DONE  (1u << 1)
#define PRCM_PDSTAT0_SERIAL_ON     (1u << 1)
#define PRCM_PDSTAT0_PERIPH_ON     (1u << 2)

/* IOC */
#define IOC_IOCFG(dio)     REG(0x40081000u + 4u * (dio))

#define IOC_PORT_ID_GPIO      0x00u
#define IOC_PORT_ID_UART0_TX  0x10u
#define IOC_PULL_CTL_DIS      (3u << 13)

/* GPIO */
#define GPIO_BASE          0x40022000u
#define GPIO_DOUTSET31_0   REG(GPIO_BASE + 0x090)
#define GPIO_DOUTCLR31_0   REG(GPIO_BASE + 0x0A0)
#define GPIO_DOE31_0       REG(GPIO_BASE + 0x0D0)

/* UART0 */
#define UART0_BASE         0x40001000u
#define UART_DR            REG(UART0_BASE + 0x000)
#define UART_FR            REG(UART0_BASE + 0x018)
#define UART_IBRD          REG(UART0_BASE + 0x024)
#define UART_FBRD          REG(UART0_BASE + 0x028)
#define UART_LCRH          REG(UART0_BASE + 0x02C)
#define UART_CTL           REG(UART0_BASE + 0x030)
#define UART_IMSC          REG(UART0_BASE + 0x038)
#define UART_ICR           REG(UART0_BASE + 0x044)

#define UART_FR_BUSY       (1u << 3)
#define UART_FR_TXFF       (1u << 5)
#define UART_LCRH_FEN      (1u << 4)
#define UART_LCRH_WLEN_8   (3u << 5)
#define UART_CTL_UARTEN    (1u << 0)
#define UART_CTL_TXE       (1u << 8)

/*
 * Watchdog. FrSky's firmware arms it; it can't be stopped once started, so
 * reload it with the longest period (~48 min at 1.5 MHz) as we go.
 */
#define WDT_BASE           0x40080000u
#define WDT_LOAD           REG(WDT_BASE + 0x000)
#define WDT_ICR            REG(WDT_BASE + 0x00C)
#define WDT_LOCK           REG(WDT_BASE + 0xC00)

/* Core */
#define SYST_CSR           REG(0xE000E010u)
#define NVIC_ICER(n)       REG(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n)       REG(0xE000E280u + 4u * (n))
#define SCB_VTOR           REG(0xE000ED08u)

static void wdt_reload(void)
{
    WDT_LOCK = 0x1ACCE551u;
    WDT_ICR  = 0;
    WDT_LOAD = 0xFFFFFFFFu;
    WDT_LOCK = 0;
}

static void power_init(void)
{
    PRCM_PDCTL0PERIPH = 1;
    PRCM_PDCTL0SERIAL = 1;
    while ((PRCM_PDSTAT0 & (PRCM_PDSTAT0_PERIPH_ON | PRCM_PDSTAT0_SERIAL_ON))
           != (PRCM_PDSTAT0_PERIPH_ON | PRCM_PDSTAT0_SERIAL_ON))
        ;

    PRCM_GPIOCLKGR = 1;
    PRCM_UARTCLKGR = 1;
    PRCM_CLKLOADCTL = PRCM_CLKLOADCTL_LOAD;
    while (!(PRCM_CLKLOADCTL & PRCM_CLKLOADCTL_LOAD_DONE))
        ;
}

static void tx_enable(int on)
{
    if (DIO_TX_ENABLE < 0)
        return;
    if (on)
        GPIO_DOUTCLR31_0 = 1u << DIO_TX_ENABLE;
    else
        GPIO_DOUTSET31_0 = 1u << DIO_TX_ENABLE;
}

static void pins_init(void)
{
    /* transmit-enable: GPIO output, high (released) to start with */
    if (DIO_TX_ENABLE >= 0) {
        tx_enable(0);
        IOC_IOCFG(DIO_TX_ENABLE) = IOC_PORT_ID_GPIO | IOC_PULL_CTL_DIS;
        GPIO_DOE31_0 |= 1u << DIO_TX_ENABLE;
    }

    IOC_IOCFG(DIO_TX) = IOC_PORT_ID_UART0_TX | IOC_PULL_CTL_DIS;
    if (DIO_TX2 >= 0)
        IOC_IOCFG(DIO_TX2) = IOC_PORT_ID_UART0_TX | IOC_PULL_CTL_DIS;
}

static void uart_init(void)
{
    /* divisor in 1/64ths: 48 MHz / (16 * 57600) = 52 + 5/64 */
    uint32_t div = (8u * CPU_FREQ / BAUDRATE + 1) / 2;

    UART_CTL  = 0;
    UART_IMSC = 0;
    UART_ICR  = 0x7FFu;
    UART_IBRD = div / 64;
    UART_FBRD = div % 64;
    UART_LCRH = UART_LCRH_WLEN_8 | UART_LCRH_FEN;   /* 8N1, FIFO on */
    UART_CTL  = UART_CTL_UARTEN | UART_CTL_TXE;
}

static void uart_putc(uint8_t c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = c;
    wdt_reload();
}

static void uart_put_u32(uint32_t v)
{
    for (int i = 0; i < 4; i++)
        uart_putc((uint8_t)(v >> (8 * i)));
}

static void uart_flush(void)
{
    while (UART_FR & UART_FR_BUSY)
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
        wdt_reload();
        __asm__ volatile ("nop");
    }
}

int main(void)
{
    wdt_reload();
    power_init();
    pins_init();
    uart_init();

    for (;;) {
        const uint8_t *p   = (const uint8_t *)DUMP_START;
        const uint8_t *end = p + DUMP_LENGTH;
        uint32_t crc = 0xFFFFFFFFu;

        tx_enable(1);

        uart_putc('F');
        uart_putc('D');
        uart_putc('M');
        uart_putc('P');
        uart_put_u32(DUMP_START);
        uart_put_u32(DUMP_LENGTH);

        while (p < end) {
            uart_putc(*p);
            crc = crc32_update(crc, *p++);
        }

        uart_put_u32(~crc);
        uart_flush();

        /* release the line for a quiet gap between rounds (~0.5 s) */
        tx_enable(0);
        delay(1000000);
    }
}

/*
 * Startup
 */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
static void (* const vectors[16])(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    /*
     * The bootloader runs TI-RTOS: make sure none of its interrupts (RTOS
     * tick, UART, RF core...) can fire into our vector table.
     */
    __asm__ volatile ("cpsid i");
    SYST_CSR = 0;
    for (int i = 0; i < 2; i++) {
        NVIC_ICER(i) = 0xFFFFFFFFu;
        NVIC_ICPR(i) = 0xFFFFFFFFu;
    }
    SCB_VTOR = (uint32_t)vectors;

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
    0, 0, 0, 0,
    Default_Handler,  /* SVCall */
    Default_Handler,  /* DebugMon */
    0,
    Default_Handler,  /* PendSV */
    Default_Handler,  /* SysTick */
};
