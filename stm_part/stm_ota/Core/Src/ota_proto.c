/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota_proto.c
  * @brief   stm_ota 的串口帧协议层 + CRC + USART1 DMA 接收。
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ota_proto.h"

#include <string.h>

#include "main.h"
#include "usart.h"
#include "stm32f4xx_hal.h"

/* ==========================================================================
 * CRC32 用到的这个宏是给 GCC/Clang 用的位域；这里不需要，略过。
 * ========================================================================== */

/* ==========================================================================
 * CRC 表
 * ==========================================================================
 * 两张表都是用一个 8 次循环按位推导出来的标准反射表，请勿手改：
 *   CRC16/MODBUS : poly 0xA001（0x8005 反射），自检向量 crc16("123456789") = 0x4B37
 *   CRC-32/zlib  : poly 0xEDB88320（0x04C11DB7 反射），
 *                  自检向量 crc32("123456789") = 0xCBF43926
 * 序号 1/2/255 的取值（0xC0C1 / 0xC181 / 0x4040）与 ESP32 侧 s_crc16_table 逐项一致。
 */

static const uint16_t s_crc16_table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,
};

static const uint32_t s_crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

uint16_t ota_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;
    while (len-- > 0U) {
        crc = (uint16_t)((crc >> 8) ^ s_crc16_table[(crc ^ *data++) & 0xFFU]);
    }
    return crc;
}

uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    /* 与 esp_rom_crc32_le 一样：进入时取反、离开时取反，
     * 于是「上次的返回值」可以直接作为「这次传入的 crc」继续累进。 */
    crc = ~crc;
    while (len-- > 0U) {
        crc = s_crc32_table[(crc ^ *data++) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t ota_crc32(const uint8_t *data, uint32_t len)
{
    return ota_crc32_update(0U, data, len);
}

/* ==========================================================================
 * USART1 环形 DMA 接收
 * ========================================================================== */

#define OTA_DMA_STREAM   DMA2_Stream5
#define OTA_DMA_CHANNEL  4U          /* USART1_RX，见 RM0090 DMA2 请求映射表 */

static uint8_t s_dma_buf[OTA_RX_DMA_BUF_SIZE];
static volatile uint16_t s_dma_pos;            /* 上次已搬走的 DMA 缓冲位置 */

static uint8_t s_ring[OTA_RX_RING_SIZE];
static volatile uint16_t s_ring_head;          /* ISR 写 */
static volatile uint16_t s_ring_tail;          /* 主循环读 */
static volatile uint32_t s_ring_dropped;
static volatile uint32_t s_rx_err_cnt;

static ota_frame_cb_t s_frame_cb;

/* DMA 环里新增的字节搬到软件环。可在 ISR 与主循环两处调用，
 * 主循环那处已经关中断，所以不存在两处同时改 s_dma_pos 的重入问题。 */
static void rx_drain(void)
{
    /* NDTR 递减计数；缓冲起点到当前写指针的距离 = 容量 - NDTR */
    uint16_t pos = (uint16_t)(OTA_RX_DMA_BUF_SIZE - OTA_DMA_STREAM->NDTR);

    while (s_dma_pos != pos) {
        const uint8_t b = s_dma_buf[s_dma_pos];
        const uint16_t next = (uint16_t)((s_ring_head + 1U) & (OTA_RX_RING_SIZE - 1U));

        s_dma_pos = (uint16_t)((s_dma_pos + 1U) & (OTA_RX_DMA_BUF_SIZE - 1U));

        if (next != s_ring_tail) {
            s_ring[s_ring_head] = b;
            s_ring_head = next;
        } else {
            /* 软件环满：丢字节。上层帧解析会因 CRC16 不匹配而重同步。 */
            s_ring_dropped++;
        }
    }
}

static void rx_drain_atomic(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    rx_drain();
    __set_PRIMASK(primask);
}

static inline bool ring_get(uint8_t *b)
{
    if (s_ring_tail == s_ring_head) {
        return false;
    }
    *b = s_ring[s_ring_tail];
    s_ring_tail = (uint16_t)((s_ring_tail + 1U) & (OTA_RX_RING_SIZE - 1U));
    return true;
}

/* ------------------------------------------------------------------ DMA 初始化 */

static void dma_rx_start(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* 改 CR/NDTR/PAR/M0AR 之前必须先关流，并等 EN 真正落下去 */
    OTA_DMA_STREAM->CR &= ~DMA_SxCR_EN;
    while ((OTA_DMA_STREAM->CR & DMA_SxCR_EN) != 0U) {
        /* 等硬件确认关闭 */
    }

    /* 清掉该流可能残留的所有中断标志（不启用 DMA 中断，但标志要干净） */
    DMA2->HIFCR = 0x0F7D0F7DU;

    OTA_DMA_STREAM->PAR  = (uint32_t)&USART1->DR;
    OTA_DMA_STREAM->M0AR = (uint32_t)s_dma_buf;
    OTA_DMA_STREAM->M1AR = 0U;
    OTA_DMA_STREAM->NDTR = OTA_RX_DMA_BUF_SIZE;
    OTA_DMA_STREAM->FCR  = DMA_SxFCR_DMDIS;   /* 直接模式（逐字节） */

    OTA_DMA_STREAM->CR = (OTA_DMA_CHANNEL << DMA_SxCR_CHSEL_Pos)
                       | DMA_SxCR_MINC      /* 存储器地址递增 */
                       | DMA_SxCR_CIRC      /* 循环模式，永不停止 */
                       | DMA_SxCR_PL_1;     /* 高优先级 */
    /* DIR=00（外设->存储器）、PSIZE=MSIZE=00（字节）、不使能任何中断 */

    OTA_DMA_STREAM->CR |= DMA_SxCR_EN;
    while ((OTA_DMA_STREAM->CR & DMA_SxCR_EN) == 0U) {
        /* 等流真正开起来 */
    }
}

/* ------------------------------------------------------------------ ISR */

void USART1_IRQHandler(void)
{
    const uint32_t sr = USART1->SR;

    if (((sr & USART_SR_IDLE) != 0U) && ((USART1->CR1 & USART_CR1_IDLEIE) != 0U)) {
        /* 清 IDLE 必须「读 SR 再读 DR」，而读 DR 会顺手把 RXNE 吃掉。
         * 这里不会丢字节：IDLE 是线路静默满 1 个帧时长（115200 下约 87µs）后
         * 才置位的，那时候最后一个字节早就被 DMA 搬走了，DR 里已经没东西。
         * 正因为有这个「静默」前提，下面错误标志才敢不读 DR —— 它们可以在
         * 数据正在流动时置位，那时 DR 里的字节 DMA 还没搬，读了就丢了。 */
        (void)USART1->DR;      /* SR 已在上面读过，这一读同时清掉 IDLE 和 RXNE */
        rx_drain();
    }

    /* 错误标志（ORE/FE/NE/PE）故意不在 ISR 里清。
     * 清它们要读 DR，而 DR 里的那一帧数据 DMA 还没搬走，ISR 抢先读会把这次
     * DMA 请求吃掉，反而丢一个字节。本协议有 SOF + CRC16，收到脏字节的后果
     * 只是丢一帧后重同步，比丢字节轻。 */
    if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U) {
        s_rx_err_cnt++;
    }
}

/* ==========================================================================
 * 帧解析
 * ==========================================================================
 * 逐字节推进的状态机：不要求缓冲连续，配环形缓冲刚好。
 */

#define RX_FRAME_CAP (OTA_HEADER_LEN + OTA_MAX_PAYLOAD + OTA_CRC_LEN)

typedef enum {
    PS_SOF0 = 0,   /* 等第一个 SOF 字节 0xAA */
    PS_SOF1,       /* 等第二个 SOF 字节 0x55 */
    PS_HEADER,     /* 收 TYPE/SEQ/LEN 共 5 字节 */
    PS_PAYLOAD,    /* 收 PAYLOAD + CRC 共 len+2 字节 */
    PS_SKIP,       /* LEN 超限，纯丢弃 */
} ps_t;

static uint8_t  s_rx_frame[RX_FRAME_CAP];
static ps_t     s_ps;
static uint16_t s_rx_got;      /* 已收进 s_rx_frame 的字节数 */
static uint16_t s_rx_need;     /* 本帧总字节数（含 SOF 与 CRC） */
static uint32_t s_rx_skip;     /* PS_SKIP 剩余丢弃字节数 */
static uint16_t s_rx_type;
static uint16_t s_rx_seq;
static uint16_t s_rx_len;
static uint32_t s_rx_bad_crc;
static uint32_t s_rx_bad_len;

static void parser_reset(void)
{
    s_ps = PS_SOF0;
    s_rx_got = 0U;
}

static void dispatch_frame(void)
{
    const uint16_t crc_calc = ota_crc16(&s_rx_frame[2], (uint32_t)(OTA_HEADER_LEN - 2U) + s_rx_len);
    const uint16_t crc_recv =
        (uint16_t)s_rx_frame[OTA_HEADER_LEN + s_rx_len] |
        (uint16_t)((uint16_t)s_rx_frame[OTA_HEADER_LEN + s_rx_len + 1U] << 8);

    if (crc_calc != crc_recv) {
        /* 对端也会用 CRC16 校验我们的帧，所以这里只要静默重同步，
         * 不需要额外报错——重传/超时由对端负责。 */
        s_rx_bad_crc++;
        return;
    }
    if (s_frame_cb != NULL) {
        s_frame_cb((uint8_t)s_rx_type, s_rx_seq, &s_rx_frame[OTA_HEADER_LEN], s_rx_len);
    }
}

void ota_proto_poll(void)
{
    uint8_t b;

    /* 主循环这条路径关中断，防止与 ISR 里的 rx_drain() 抢 s_dma_pos */
    rx_drain_atomic();

    for (;;) {
        switch (s_ps) {
        case PS_SOF0:
            if (!ring_get(&b)) {
                return;
            }
            if (b == OTA_SOF_0) {
                s_rx_frame[0] = b;
                s_ps = PS_SOF1;
            }
            break;

        case PS_SOF1:
            if (!ring_get(&b)) {
                return;
            }
            if (b == OTA_SOF_1) {
                s_rx_frame[1] = b;
                s_rx_got = 2U;
                s_ps = PS_HEADER;
            } else if (b != OTA_SOF_0) {
                s_ps = PS_SOF0;      /* 不是 0xAA 也不是 0x55，重来 */
            }
            /* b == 0xAA 时留在本状态，等下一个 0x55 */
            break;

        case PS_HEADER:
            while (s_rx_got < OTA_HEADER_LEN) {
                if (!ring_get(&b)) {
                    return;
                }
                s_rx_frame[s_rx_got++] = b;
            }
            s_rx_type = s_rx_frame[2];
            s_rx_seq = (uint16_t)s_rx_frame[3] | (uint16_t)((uint16_t)s_rx_frame[4] << 8);
            s_rx_len = (uint16_t)s_rx_frame[5] | (uint16_t)((uint16_t)s_rx_frame[6] << 8);
            if (s_rx_len > OTA_MAX_PAYLOAD) {
                /* LEN 超限：丢掉整帧剩余部分，别把缓冲写炸 */
                s_rx_bad_len++;
                s_rx_skip = (uint32_t)s_rx_len + OTA_CRC_LEN;
                s_ps = PS_SKIP;
            } else {
                s_rx_need = (uint16_t)(OTA_HEADER_LEN + s_rx_len + OTA_CRC_LEN);
                s_ps = PS_PAYLOAD;
            }
            break;

        case PS_PAYLOAD:
            while (s_rx_got < s_rx_need) {
                if (!ring_get(&b)) {
                    return;
                }
                s_rx_frame[s_rx_got++] = b;
            }
            s_ps = PS_SOF0;          /* 先复位，回调里可能重入发帧 */
            dispatch_frame();
            break;

        case PS_SKIP:
            while (s_rx_skip > 0U) {
                if (!ring_get(&b)) {
                    return;
                }
                s_rx_skip--;
            }
            s_ps = PS_SOF0;
            break;

        default:
            s_ps = PS_SOF0;
            break;
        }
    }
}

void ota_proto_resync(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    parser_reset();
    s_ring_tail = s_ring_head;     /* 丢掉未处理的残余 */
    __set_PRIMASK(primask);
}

void ota_proto_get_diag(uint32_t *dropped, uint32_t *bad_crc, uint32_t *bad_len,
                        uint32_t *errors)
{
    if (dropped != NULL) {
        *dropped = s_ring_dropped;
    }
    if (bad_crc != NULL) {
        *bad_crc = s_rx_bad_crc;
    }
    if (bad_len != NULL) {
        *bad_len = s_rx_bad_len;
    }
    if (errors != NULL) {
        *errors = s_rx_err_cnt;
    }
}

/* ==========================================================================
 * 发送
 * ========================================================================== */

#define OTA_TX_TIMEOUT_MS 100U

static uint8_t s_tx_frame[OTA_HEADER_LEN + OTA_MAX_PAYLOAD + OTA_CRC_LEN];

bool ota_proto_send(uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    if (len > OTA_MAX_PAYLOAD) {
        return false;
    }

    s_tx_frame[0] = OTA_SOF_0;
    s_tx_frame[1] = OTA_SOF_1;
    s_tx_frame[2] = type;
    s_tx_frame[3] = (uint8_t)(seq & 0xFFU);
    s_tx_frame[4] = (uint8_t)(seq >> 8);
    s_tx_frame[5] = (uint8_t)(len & 0xFFU);
    s_tx_frame[6] = (uint8_t)(len >> 8);
    if (len > 0U && payload != NULL) {
        memcpy(&s_tx_frame[OTA_HEADER_LEN], payload, len);
    }

    /* CRC16 覆盖 TYPE..PAYLOAD，不含 SOF */
    const uint16_t crc =
        ota_crc16(&s_tx_frame[2], (uint32_t)(OTA_HEADER_LEN - 2U) + len);
    s_tx_frame[OTA_HEADER_LEN + len] = (uint8_t)(crc & 0xFFU);
    s_tx_frame[OTA_HEADER_LEN + len + 1U] = (uint8_t)(crc >> 8);

    const uint16_t total = (uint16_t)(OTA_HEADER_LEN + len + OTA_CRC_LEN);
    return HAL_UART_Transmit(&huart1, s_tx_frame, total, OTA_TX_TIMEOUT_MS) == HAL_OK;
}

bool ota_proto_send_ack(uint16_t ack_seq, uint8_t status)
{
    const uint8_t payload[3] = {
        (uint8_t)(ack_seq & 0xFFU),
        (uint8_t)(ack_seq >> 8),
        status,
    };
    return ota_proto_send(OTA_FRAME_ACK, 0U, payload, sizeof(payload));
}

bool ota_proto_send_crc_result(uint8_t status)
{
    return ota_proto_send(OTA_FRAME_CRC_RESULT, 0U, &status, 1U);
}

/* ---- 手写 JSON：故意不用 snprintf ----
 * 在 64KB 的 Bootloader 里，newlib 的 printf 家族会拖进十几 KB 的代码，
 * 而这里只需要拼几个固定字段，手写反而更小更可控。 */

static char *json_lit(char *p, char *end, const char *s)
{
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

static char *json_str(char *p, char *end, const char *s)
{
    while (*s != '\0' && p < end) {
        const char c = *s++;
        if (c == '"' || c == '\\') {
            if (p + 1 >= end) {
                break;
            }
            *p++ = '\\';
        }
        *p++ = c;
    }
    return p;
}

static char *json_u32(char *p, char *end, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0U;

    if (v == 0U) {
        tmp[n++] = '0';
    }
    while (v > 0U && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0U && p < end) {
        *p++ = tmp[--n];
    }
    return p;
}

bool ota_proto_status_json(const char *stage, int percent, const char *msg)
{
    char buf[OTA_STATUS_JSON_MAX];
    char *end = &buf[sizeof(buf) - 1U];   /* 最后 1 字节留给收尾的 '}' */
    char *p = buf;

    *p++ = '{';
    p = json_lit(p, end, "\"ota_stage\":\"");
    p = json_str(p, end, (stage != NULL) ? stage : "");
    p = json_lit(p, end, "\",\"ota_percent\":");
    if (percent < 0) {
        p = json_lit(p, end, "-1");
    } else {
        p = json_u32(p, end, (uint32_t)percent);
    }
    if (msg != NULL) {
        p = json_lit(p, end, ",\"msg\":\"");
        p = json_str(p, end, msg);
        p = json_lit(p, end, "\"");
    }
    *p++ = '}';

    return ota_proto_send(OTA_FRAME_STM_STATUS, 0U, buf, (uint16_t)(p - buf));
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */

void ota_proto_register_rx(ota_frame_cb_t cb)
{
    s_frame_cb = cb;
}

bool ota_proto_init(void)
{
    /* 1. CRC 自检。上电就把结论打出来，联调时不用猜是哪一侧算法不对。
     *    两个标准测试向量：CRC32("123456789")=0xCBF43926、CRC16/MODBUS("123456789")=0x4B37。
     *    只要有一张表被抄错，这里立刻就能定位，不用等联调时看到「每帧都 CRC 不匹配」。 */
    static const uint8_t kCheck[] = "123456789";
    const uint32_t crc32_v = ota_crc32(kCheck, (uint32_t)(sizeof(kCheck) - 1U));
    const uint16_t crc16_v = ota_crc16(kCheck, (uint32_t)(sizeof(kCheck) - 1U));
    const bool crc_ok = (crc32_v == 0xCBF43926U) && (crc16_v == 0x4B37U);

    /* 2. 清掉可能残留的 RXNE，免得 DMA 一开就把陈年字节搬进来 */
    (void)USART1->SR;
    (void)USART1->DR;

    /* 3. 屏蔽 USART1 自身的中断位，只留 IDLE；错误标志交给 CRC 兜底 */
    USART1->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_PEIE | USART_CR1_TXEIE | USART_CR1_TCIE);
    USART1->CR3 &= ~USART_CR3_EIE;

    /* 4. 先让 DMA 流跑起来，再打开 USART 的 DMA 接收请求 */
    dma_rx_start();
    USART1->CR3 |= USART_CR3_DMAR;

    /* 5. IDLE 行中断：一帧结束后把 DMA 里的数据搬进软件环 */
    USART1->CR1 |= USART_CR1_IDLEIE;
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    s_ps = PS_SOF0;
    s_rx_got = 0U;
    s_dma_pos = 0U;
    s_ring_head = 0U;
    s_ring_tail = 0U;

    return crc_ok;
}
