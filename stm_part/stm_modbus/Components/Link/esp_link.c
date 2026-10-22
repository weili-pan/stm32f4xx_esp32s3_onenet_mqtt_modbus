#include "esp_link.h"

#include "usart.h"     /* huart1 */

/*----------------------------------------------- CRC16/MODBUS ---------------------------------------
 * 逐位实现，与 ESP32 侧 connect_stm 的表驱动版本结果完全一致
 * （标准向量 crc16("123456789") = 0x4B37）。帧率 ~1Hz，这点开销无所谓，
 * 换来的是不用在 App 里再塞一张 256 项的常量表。
 */
static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    while (len-- > 0U)
    {
        crc ^= (uint16_t)(*data++);
        for (uint8_t i = 0U; i < 8U; i++)
        {
            if ((crc & 0x0001U) != 0U)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

/*----------------------------------------------- 接收环形缓冲 ---------------------------------------
 * 中断里写 head，主循环里读 tail。大小为 2 的幂，用掩码取模。
 */

static volatile uint8_t  s_rx[ESP_LINK_RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

static inline bool ring_get(uint8_t *b)
{
    if (s_rx_tail == s_rx_head)
    {
        return false;
    }
    *b = s_rx[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) & (ESP_LINK_RX_RING_SIZE - 1U));
    return true;
}

/*----------------------------------------------- 接收中断 -------------------------------------------*/

void USART1_IRQHandler(void)
{
    const uint32_t sr = USART1->SR;

    if ((sr & USART_SR_RXNE) != 0U)
    {
        const uint8_t  b    = (uint8_t)(USART1->DR & 0xFFU);   /* 读 DR 同时清 RXNE */
        const uint16_t next = (uint16_t)((s_rx_head + 1U) & (ESP_LINK_RX_RING_SIZE - 1U));

        if (next != s_rx_tail)      /* 满了就丢这个字节，交给 CRC16 重同步 */
        {
            s_rx[s_rx_head] = b;
            s_rx_head       = next;
        }
    }
    else if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U)
    {
        (void)USART1->DR;           /* 先读 SR 再读 DR 才能清掉这些错误标志 */
    }
}

/*----------------------------------------------- 帧解析状态机 ---------------------------------------*/

#define RX_FRAME_CAP (ESP_LINK_HEADER_LEN + ESP_LINK_MAX_PAYLOAD + ESP_LINK_CRC_LEN)

typedef enum {
    PS_SOF0 = 0,   /* 等第一个 SOF 字节 0xAA */
    PS_SOF1,       /* 等第二个 SOF 字节 0x55 */
    PS_HEADER,     /* 收 TYPE/SEQ/LEN 共 5 字节 */
    PS_PAYLOAD,    /* 收 PAYLOAD + CRC 共 len+2 字节 */
    PS_SKIP,       /* LEN 超限，纯丢弃 */
} ps_t;

static uint8_t  s_frame[RX_FRAME_CAP];
static ps_t     s_ps;
static uint16_t s_got;         /* 已收字节数 */
static uint16_t s_need;        /* 本帧总字节数（含 SOF 与 CRC） */
static uint32_t s_skip;        /* PS_SKIP 剩余丢弃字节数 */
static uint16_t s_type;
static uint16_t s_len;

/* 最新状态快照 + 最近一次收到有效 0x05 帧的时刻 */
static esp_link_status_t s_status;
static volatile uint32_t s_last_rx_tick;
static uint8_t           s_ever_rx;

static void dispatch_frame(void)
{
    /* CRC16 覆盖 TYPE..PAYLOAD（从第 2 字节起，共 HEADER_LEN-2 + len 字节） */
    const uint16_t crc_calc =
        crc16_modbus(&s_frame[2], (uint32_t)(ESP_LINK_HEADER_LEN - 2U) + s_len);
    const uint16_t crc_recv =
        (uint16_t)s_frame[ESP_LINK_HEADER_LEN + s_len] |
        (uint16_t)((uint16_t)s_frame[ESP_LINK_HEADER_LEN + s_len + 1U] << 8);

    if (crc_calc != crc_recv)
    {
        return;   /* 静默丢弃，等重同步 */
    }

    if ((s_type == ESP_LINK_FRAME_LINK_STATUS) &&
        (s_len == sizeof(esp_link_status_t)))
    {
        const uint8_t *p = &s_frame[ESP_LINK_HEADER_LEN];
        s_status.wifi      = p[0];
        s_status.mqtt      = p[1];
        s_status.rssi      = (int8_t)p[2];
        s_status.devices   = p[3];
        s_status.ota_ready = p[4];
        s_last_rx_tick     = HAL_GetTick();
        s_ever_rx          = 1U;
    }
    /* 其它类型本工程不处理，直接忽略 */
}

void EspLink_Poll(void)
{
    uint8_t b;

    for (;;)
    {
        switch (s_ps)
        {
        case PS_SOF0:
            if (!ring_get(&b)) { return; }
            if (b == ESP_LINK_SOF_0)
            {
                s_frame[0] = b;
                s_ps = PS_SOF1;
            }
            break;

        case PS_SOF1:
            if (!ring_get(&b)) { return; }
            if (b == ESP_LINK_SOF_1)
            {
                s_frame[1] = b;
                s_got = 2U;
                s_ps  = PS_HEADER;
            }
            else if (b != ESP_LINK_SOF_0)
            {
                s_ps = PS_SOF0;   /* 不是 0xAA 也不是 0x55，重来 */
            }
            /* b == 0xAA 时留在本状态，等下一个 0x55 */
            break;

        case PS_HEADER:
            while (s_got < ESP_LINK_HEADER_LEN)
            {
                if (!ring_get(&b)) { return; }
                s_frame[s_got++] = b;
            }
            s_type = s_frame[2];
            s_len  = (uint16_t)s_frame[5] | (uint16_t)((uint16_t)s_frame[6] << 8);
            if (s_len > ESP_LINK_MAX_PAYLOAD)
            {
                s_skip = (uint32_t)s_len + ESP_LINK_CRC_LEN;
                s_ps   = PS_SKIP;
            }
            else
            {
                s_need = (uint16_t)(ESP_LINK_HEADER_LEN + s_len + ESP_LINK_CRC_LEN);
                s_ps   = PS_PAYLOAD;
            }
            break;

        case PS_PAYLOAD:
            while (s_got < s_need)
            {
                if (!ring_get(&b)) { return; }
                s_frame[s_got++] = b;
            }
            s_ps = PS_SOF0;
            dispatch_frame();
            break;

        case PS_SKIP:
            while (s_skip > 0U)
            {
                if (!ring_get(&b)) { return; }
                s_skip--;
            }
            s_ps = PS_SOF0;
            break;

        default:
            s_ps = PS_SOF0;
            break;
        }
    }
}

/*----------------------------------------------- 初始化 -------------------------------------------*/

void EspLink_Init(void)
{
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_ps      = PS_SOF0;
    s_got     = 0U;
    s_ever_rx = 0U;
    s_last_rx_tick = 0U;
    s_status.wifi = 0U;
    s_status.mqtt = 0U;
    s_status.rssi = 0;
    s_status.devices = 0U;
    s_status.ota_ready = 0U;

    /* 清掉可能残留的 RXNE，免得一开中断就吃进一个陈年字节 */
    (void)USART1->SR;
    (void)USART1->DR;

    /* huart1 已由 CubeMX 的 MX_USART1_UART_Init() 配好（115200 8N1），这里只挂中断 */
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

/*----------------------------------------------- 查询 -------------------------------------------*/

bool EspLink_GetStatus(esp_link_status_t *out)
{
    if (out != NULL)
    {
        *out = s_status;
    }
    return (s_ever_rx != 0U) &&
           ((HAL_GetTick() - s_last_rx_tick) < ESP_LINK_LINK_TIMEOUT_MS);
}
