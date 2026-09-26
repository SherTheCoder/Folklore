#include "net/net_eth.h"
#include "app/kikkar_config.h"
#include "main.h"
#include "stm32n6xx_hal_rif.h"
#include <string.h>

#define PHY_ADDR            0x01u
#define PHY_BMCR            0x00u
#define PHY_BMSR            0x01u
#define PHY_BMCR_RESET      0x8000u
#define PHY_BMCR_ANEG       0x1000u
#define PHY_BMCR_ANEG_START 0x0200u
#define PHY_BMSR_LINK       0x0004u
#define PHY_BMSR_ANEG_DONE  0x0020u
#define PHY_RTL_PHYSR       0x1Au
#define PHY_RTL_LINK        0x0004u
#define PHY_RTL_DUPLEX      0x0008u
#define PHY_RTL_SPEED_MASK  0x0030u
#define PHY_RTL_SPEED_10    0x0000u
#define PHY_RTL_SPEED_100   0x0010u
#define PHY_RTL_SPEED_1000  0x0020u

#define DESC_OWN            0x80000000u
#define TX_DES3_FD          0x20000000u
#define TX_DES3_LD          0x10000000u
#define TX_DES2_IOC         0x80000000u
#define RX_DES3_IOC         0x40000000u
#define RX_DES3_BUF1V       0x01000000u
#define DESC_LEN_MASK       0x00007FFFu

#define NONCACHEABLE __attribute__((section(".noncacheable")))

typedef struct
{
    volatile uint32_t des0;
    volatile uint32_t des1;
    volatile uint32_t des2;
    volatile uint32_t des3;
} eth_desc_t;

static eth_desc_t s_tx_desc[NET_TX_DESC] __attribute__((aligned(32))) NONCACHEABLE;
static eth_desc_t s_rx_desc[NET_RX_DESC] __attribute__((aligned(32))) NONCACHEABLE;
static uint8_t    s_tx_buf[NET_TX_DESC][NET_FRAME_MAX] __attribute__((aligned(4))) NONCACHEABLE;
static uint8_t    s_rx_buf[NET_RX_DESC][NET_FRAME_MAX] __attribute__((aligned(4))) NONCACHEABLE;

static uint8_t    s_mac[NET_ETH_ADDR_LEN];
static uint32_t   s_tx_head;
static uint32_t   s_rx_tail;
static net_link_t s_link;
static uint32_t   s_tx_frames;
static uint32_t   s_rx_frames;
static uint32_t   s_tx_drops;
static uint32_t   s_errors;

/* RGMII pins as in Kikkar.ioc, all very high speed for the 125 MHz clock. */
static void eth_pins_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_ETH1;

    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_5 | GPIO_PIN_7 | GPIO_PIN_8 |
               GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
               GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOF, &gpio);

    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_4;
    HAL_GPIO_Init(GPIOG, &gpio);
}

static void eth_master_attributes(void)
{
    RIMC_MasterConfig_t cfg;

    cfg.MasterCID = RIF_CID_1;
    cfg.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_ETH1, &cfg);
}

static uint16_t phy_read(uint8_t reg)
{
    uint32_t timeout = 100000u;

    ETH1->MACMDIOAR = (ETH1->MACMDIOAR & ETH_MACMDIOAR_CR) |
                      ((uint32_t)PHY_ADDR << ETH_MACMDIOAR_PA_Pos) |
                      ((uint32_t)reg << ETH_MACMDIOAR_RDA_Pos) |
                      ETH_MACMDIOAR_GOC_0 | ETH_MACMDIOAR_GOC_1 |
                      ETH_MACMDIOAR_GB;

    while ((ETH1->MACMDIOAR & ETH_MACMDIOAR_GB) != 0u && --timeout != 0u)
    {
    }
    return (uint16_t)(ETH1->MACMDIODR & 0xFFFFu);
}

static void phy_write(uint8_t reg, uint16_t value)
{
    uint32_t timeout = 100000u;

    ETH1->MACMDIODR = value;
    ETH1->MACMDIOAR = (ETH1->MACMDIOAR & ETH_MACMDIOAR_CR) |
                      ((uint32_t)PHY_ADDR << ETH_MACMDIOAR_PA_Pos) |
                      ((uint32_t)reg << ETH_MACMDIOAR_RDA_Pos) |
                      ETH_MACMDIOAR_GOC_0 |
                      ETH_MACMDIOAR_GB;

    while ((ETH1->MACMDIOAR & ETH_MACMDIOAR_GB) != 0u && --timeout != 0u)
    {
    }
}

static void rings_init(void)
{
    uint32_t i;

    for (i = 0u; i < NET_TX_DESC; i++)
    {
        s_tx_desc[i].des0 = (uint32_t)(uintptr_t)s_tx_buf[i];
        s_tx_desc[i].des1 = 0u;
        s_tx_desc[i].des2 = 0u;
        s_tx_desc[i].des3 = 0u;
    }
    for (i = 0u; i < NET_RX_DESC; i++)
    {
        s_rx_desc[i].des0 = (uint32_t)(uintptr_t)s_rx_buf[i];
        s_rx_desc[i].des1 = 0u;
        s_rx_desc[i].des2 = 0u;
        s_rx_desc[i].des3 = DESC_OWN | RX_DES3_IOC | RX_DES3_BUF1V;
    }
    s_tx_head = 0u;
    s_rx_tail = 0u;

    ETH1->DMA_CH[0].DMACTXDLAR = (uint32_t)(uintptr_t)s_tx_desc;
    ETH1->DMA_CH[0].DMACTXRLR  = NET_TX_DESC - 1u;
    ETH1->DMA_CH[0].DMACTXDTPR = (uint32_t)(uintptr_t)&s_tx_desc[0];

    ETH1->DMA_CH[0].DMACRXDLAR = (uint32_t)(uintptr_t)s_rx_desc;
    ETH1->DMA_CH[0].DMACRXRLR  = NET_RX_DESC - 1u;
    ETH1->DMA_CH[0].DMACRXDTPR = (uint32_t)(uintptr_t)&s_rx_desc[NET_RX_DESC - 1u];
}

static void mac_set_mode(net_link_t link, int full_duplex)
{
    uint32_t cr = ETH1->MACCR;

    cr &= ~(ETH_MACCR_PS | ETH_MACCR_FES | ETH_MACCR_DM);
    if (link == NET_LINK_100)
    {
        cr |= ETH_MACCR_PS | ETH_MACCR_FES;
    }
    else if (link == NET_LINK_10)
    {
        cr |= ETH_MACCR_PS;
    }
    if (full_duplex)
    {
        cr |= ETH_MACCR_DM;
    }
    ETH1->MACCR = cr;
}

int net_eth_init(const uint8_t mac[NET_ETH_ADDR_LEN])
{
    uint32_t timeout = 1000000u;

    memcpy(s_mac, mac, NET_ETH_ADDR_LEN);

    eth_pins_init();

    __HAL_RCC_ETH1_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();
    __HAL_RCC_ETH1MAC_CLK_ENABLE();

    eth_master_attributes();

    ETH1->DMAMR |= ETH_DMAMR_SWR;
    while ((ETH1->DMAMR & ETH_DMAMR_SWR) != 0u && --timeout != 0u)
    {
    }
    if (timeout == 0u)
    {
        s_errors++;
        return -1;
    }

    ETH1->MACMDIOAR = ETH_MACMDIOAR_CR_DIV124;

    phy_write(PHY_BMCR, PHY_BMCR_RESET);
    for (timeout = 0u; timeout < 100000u; timeout++)
    {
        if ((phy_read(PHY_BMCR) & PHY_BMCR_RESET) == 0u)
        {
            break;
        }
    }
    phy_write(PHY_BMCR, PHY_BMCR_ANEG | PHY_BMCR_ANEG_START);

    ETH1->DMASBMR |= ETH_DMASBMR_FB | ETH_DMASBMR_AAL;
    ETH1->DMA_CH[0].DMACCR   = 0u;
    ETH1->DMA_CH[0].DMACTXCR = (32u << ETH_DMACxTXCR_TXPBL_Pos);
    ETH1->DMA_CH[0].DMACRXCR = (32u << ETH_DMACxRXCR_RXPBL_Pos) |
                               (NET_FRAME_MAX << ETH_DMACxRXCR_RBSZ_Pos);

    ETH1->MTL_QUEUE[0].MTLTXQOMR = ETH_MTLTXQxOMR_TSF | ETH_MTLTXQxOMR_TXQEN_EN;
    ETH1->MTL_QUEUE[0].MTLRXQOMR = ETH_MTLRXQxOMR_RSF;

    ETH1->MACA0HR = ((uint32_t)s_mac[5] << 8) | (uint32_t)s_mac[4] | ETH_MACA0HR_AE;
    ETH1->MACA0LR = ((uint32_t)s_mac[3] << 24) | ((uint32_t)s_mac[2] << 16) |
                    ((uint32_t)s_mac[1] << 8)  | (uint32_t)s_mac[0];

    rings_init();

    ETH1->MACCR |= ETH_MACCR_TE | ETH_MACCR_RE | ETH_MACCR_ACS |
                   ETH_MACCR_WD | ETH_MACCR_JD;
    ETH1->DMA_CH[0].DMACTXCR |= ETH_DMACxTXCR_ST;
    ETH1->DMA_CH[0].DMACRXCR |= ETH_DMACxRXCR_SR;

    s_link = NET_LINK_DOWN;
    return 0;
}

net_link_t net_eth_poll_link(void)
{
    uint16_t bmsr = phy_read(PHY_BMSR);
    uint16_t physr;
    net_link_t link;

    if ((bmsr & PHY_BMSR_LINK) == 0u)
    {
        s_link = NET_LINK_DOWN;
        return s_link;
    }

    physr = phy_read(PHY_RTL_PHYSR);
    switch (physr & PHY_RTL_SPEED_MASK)
    {
    case PHY_RTL_SPEED_1000: link = NET_LINK_1000; break;
    case PHY_RTL_SPEED_100:  link = NET_LINK_100;  break;
    default:                 link = NET_LINK_10;   break;
    }

    if (link != s_link)
    {
        mac_set_mode(link, (physr & PHY_RTL_DUPLEX) != 0u);
        s_link = link;
    }
    return s_link;
}

net_link_t net_eth_link(void)
{
    return s_link;
}

int net_eth_send(const uint8_t *frame, uint32_t len)
{
    eth_desc_t *d = &s_tx_desc[s_tx_head];

    if (s_link == NET_LINK_DOWN || len == 0u || len > NET_FRAME_MAX)
    {
        s_tx_drops++;
        return -1;
    }
    if ((d->des3 & DESC_OWN) != 0u)
    {
        s_tx_drops++;
        return -2;
    }

    memcpy(s_tx_buf[s_tx_head], frame, len);
    __DSB();

    d->des0 = (uint32_t)(uintptr_t)s_tx_buf[s_tx_head];
    d->des1 = 0u;
    d->des2 = TX_DES2_IOC | (len & 0x3FFFu);
    d->des3 = DESC_OWN | TX_DES3_FD | TX_DES3_LD | (len & DESC_LEN_MASK);
    __DSB();

    s_tx_head = (s_tx_head + 1u) % NET_TX_DESC;
    ETH1->DMA_CH[0].DMACTXDTPR = (uint32_t)(uintptr_t)&s_tx_desc[s_tx_head];

    s_tx_frames++;
    return 0;
}

uint32_t net_eth_recv(const uint8_t **frame)
{
    eth_desc_t *d = &s_rx_desc[s_rx_tail];
    uint32_t len;

    if ((d->des3 & DESC_OWN) != 0u)
    {
        return 0u;
    }

    len = d->des3 & DESC_LEN_MASK;
    if (len > NET_FRAME_MAX)
    {
        len = NET_FRAME_MAX;
    }
    *frame = s_rx_buf[s_rx_tail];
    s_rx_frames++;
    return len;
}

void net_eth_recv_done(void)
{
    eth_desc_t *d = &s_rx_desc[s_rx_tail];

    d->des0 = (uint32_t)(uintptr_t)s_rx_buf[s_rx_tail];
    d->des1 = 0u;
    d->des2 = 0u;
    __DSB();
    d->des3 = DESC_OWN | RX_DES3_IOC | RX_DES3_BUF1V;
    __DSB();

    ETH1->DMA_CH[0].DMACRXDTPR = (uint32_t)(uintptr_t)&s_rx_desc[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1u) % NET_RX_DESC;
}

const uint8_t *net_eth_mac(void)
{
    return s_mac;
}

uint32_t net_eth_tx_frames(void) { return s_tx_frames; }
uint32_t net_eth_rx_frames(void) { return s_rx_frames; }
uint32_t net_eth_tx_drops(void)  { return s_tx_drops;  }
uint32_t net_eth_errors(void)    { return s_errors;    }
