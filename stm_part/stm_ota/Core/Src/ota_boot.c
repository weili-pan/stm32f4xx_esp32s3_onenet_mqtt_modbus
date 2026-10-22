/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota_boot.c
  * @brief   stm_ota Bootloader 主流程。
  *
  *          职责：接 ESP32S3 送来的固件 -> 暂存 W25Q64 -> 校验 CRC32 ->
  *                搬运到内部 App 区 0x08010000 -> 校验 -> 跳转。
  *
  *          掉电安全的核心：W25Q64 的 OTA 头里有个 state 字段，
  *          只有**整包 CRC32 校验通过**之后才写 READY，搬运阶段只认 READY。
  *          所以任何时刻掉电，最坏结果都是「重收一次」，绝不会留下半个 App。
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ota_boot.h"

#include <string.h>

#include "main.h"
#include "stm32f4xx_hal.h"
#include "ota_config.h"
#include "ota_proto.h"
#include "w25q64.h"

/* 预擦除逻辑假设「暂存块大小 == W25Q64 扇区大小」，这里把它钉死在编译期 */
_Static_assert(OTA_STAGING_SIZE == W25Q64_SECTOR_SIZE,
               "OTA_STAGING_SIZE 必须等于 W25Q64_SECTOR_SIZE，flush 的预擦除逻辑依赖这一点");
_Static_assert((OTA_RX_RING_SIZE & (OTA_RX_RING_SIZE - 1U)) == 0U,
               "OTA_RX_RING_SIZE 必须是 2 的幂");
_Static_assert((OTA_RX_DMA_BUF_SIZE & (OTA_RX_DMA_BUF_SIZE - 1U)) == 0U,
               "OTA_RX_DMA_BUF_SIZE 必须是 2 的幂");
/* 擦除水位以扇区为单位向上推进，所以固件区起点必须落在扇区边界上 */
_Static_assert((W25Q64_OTA_FW_ADDR % W25Q64_SECTOR_SIZE) == 0U,
               "W25Q64_OTA_FW_ADDR 必须扇区对齐");

/* STM32F407 内部 Flash 扇区 5 的起始地址与大小。App 区从扇区 4 开始：
 *   扇区 4 : 0x08010000..0x0801FFFF，64KB
 *   扇区 5..11: 0x08020000 起，每个 128KB
 * 只在本文件用，所以不放进 ota_config.h。 */
#define APP_SECTOR5_BASE   0x08020000U
#define APP_SECTOR5_SIZE   0x00020000U

/* ==========================================================================
 * W25Q64 上的 OTA 头
 * ========================================================================== */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* OTA_HDR_MAGIC */
    uint32_t state;        /* OTA_HDR_STATE_* */
    uint32_t total_size;   /* 固件字节数 */
    uint32_t crc32;        /* 整包 CRC32（来自 OTA_FINISH） */
    uint32_t chunk_size;   /* 分片大小，仅作记录 */
    uint8_t  version_len;
    uint8_t  version[OTA_VERSION_MAX];
    uint8_t  reserved[3];  /* 补齐到 4 字节边界，读数好看 */
    uint32_t hdr_crc;      /* 本字段置 0 时，前面所有字节的 CRC32 */
} ota_header_t;

/* 60 字节，一个扇区装得下，外加 4036 字节空闲——够以后加字段 */
_Static_assert(sizeof(ota_header_t) <= W25Q64_SECTOR_SIZE, "OTA 头必须能塞进一个扇区");

/* ==========================================================================
 * 会话上下文
 * ========================================================================== */

static ota_header_t s_hdr;

static bool s_flash_ok;      /* W25Q64 已通过 JEDEC ID 自检 */
static bool s_flash_bad;     /* 本次会话落盘出错，数据不可信 */

static uint16_t s_expect_seq;   /* 期望的下一个 OTA_DATA 序号 */
static uint32_t s_received;     /* 已收到的固件字节数 */
static uint32_t s_crc;          /* 流式整包 CRC32 */
static uint32_t s_stage_addr;   /* 暂存区下一个要写到的 W25Q64 偏移 */
static uint32_t s_erased_upto;  /* 已擦除到的 W25Q64 偏移（不含），扇区边界对齐 */
static uint32_t s_staging_len;
static uint8_t  s_staging[OTA_STAGING_SIZE];

static bool s_have_start;       /* 本会话已经接受了 OTA_START */
static bool s_finish_seen;      /* 收到过 OTA_FINISH */
static bool s_session_end;      /* 对端发来 OTA_ABORT，本次会话结束 */
static uint32_t s_last_activity_ms;

/* 定义在文件后半段（搬运那一节），download_session 先用 */
static bool install_firmware(const ota_header_t *h);

/* ==========================================================================
 * 小工具
 * ========================================================================== */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ==========================================================================
 * 升级请求标志（RTC 备份寄存器）
 * ==========================================================================
 * 由 App 在「收到云端升级通知」时写 RTC->BKP0R = OTA_BOOT_MAGIC 然后复位。
 * 用备份寄存器而不是 Flash 扇区：不占空间、不用擦除、系统复位不会丢。
 *
 * 注意：main.c 的 USER CODE BEGIN SysInit 里已经开了 PWR 时钟和备份域写权限，
 * 否则这里读写 BKP0R 都是无效的。
 */

static bool boot_requested(void)
{
    if (OTA_BOOT_WAIT_MS > 0U) {
        return true;   /* 台架调试：每次上电都进下载模式 */
    }
    return RTC->BKP0R == OTA_BOOT_MAGIC;
}

static void clear_boot_request(void)
{
    if (RTC->BKP0R == OTA_BOOT_MAGIC) {
        RTC->BKP0R = 0U;
    }
}

/* ==========================================================================
 * App 区校验与跳转
 * ========================================================================== */

static bool app_valid(void)
{
    const uint32_t sp = *(volatile uint32_t *)OTA_APP_ADDR;
    const uint32_t pc = *(volatile uint32_t *)(OTA_APP_ADDR + 4U);

    /* 栈顶必须落在 SRAM(128KB @0x20000000) 或 CCM(64KB @0x10000000) 里，且 8 字节对齐 */
    const bool sp_ok = (((sp >= 0x20000000U) && (sp <= 0x20020000U)) ||
                        ((sp >= 0x10000000U) && (sp <= 0x10010000U))) &&
                       ((sp & 0x7U) == 0U);
    /* 复位向量必须指向 App 区，且是 Thumb 地址（最低位为 1） */
    const bool pc_ok = (pc >= OTA_APP_ADDR) && (pc < OTA_FLASH_END) && ((pc & 0x1U) != 0U);

    return sp_ok && pc_ok;
}

static void jump_to_app(void)
{
    if (!app_valid()) {
        return;   /* 双保险：调用方已经判过，绝不往空白区跳 */
    }

    const uint32_t sp = *(volatile uint32_t *)OTA_APP_ADDR;
    const uint32_t pc = *(volatile uint32_t *)(OTA_APP_ADDR + 4U);

    /* 先把升级请求标志清掉（要在 HAL_RCC_DeInit 之前，否则备份域写权限没了），
     * 否则 App 里一复位就又回到 Bootloader 的下载模式，来回循环。 */
    clear_boot_request();

    /* 关中断 -> 复位外设 -> 清 NVIC -> 换向量表和栈 -> 跳。
     * 顺序不能乱：必须在换 MSP 之前关中断，否则挂起的中断会拿新栈去跑
     * Bootloader 的向量表。 */
    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    for (uint32_t i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    HAL_RCC_DeInit();

    SCB->VTOR = OTA_APP_ADDR;
    __set_MSP(sp);

    /* 到这一步栈已经是 App 的了，不能再碰本函数里的局部变量和返回值。
     * 必须重新开中断：PRIMASK 只有系统复位才会清，而 App 的 HAL_Init/HAL_Delay
     * 都靠中断跑时间基准。之前已清掉所有挂起位，这里开中断是安全的。 */
    __enable_irq();

    ((void (*)(void))pc)();

    for (;;) {
        /* 走到这里说明 App 的复位向量直接返回了，不该发生 */
    }
}

/* ==========================================================================
 * W25Q64 上的 OTA 头读写
 * ========================================================================== */

static bool hdr_read(ota_header_t *h)
{
    if (!w25q64_read(W25Q64_OTA_HDR_ADDR, h, (uint32_t)sizeof(*h))) {
        return false;
    }
    if (h->magic != OTA_HDR_MAGIC) {
        return false;   /* 从来没写过（空白扇区是 0xFF） */
    }

    /* hdr_crc 字段本身不参与校验 */
    const uint32_t saved = h->hdr_crc;
    h->hdr_crc = 0U;
    const uint32_t calc = ota_crc32((const uint8_t *)h, (uint32_t)sizeof(*h));
    h->hdr_crc = saved;

    return calc == saved;
}

static bool hdr_write(ota_header_t *h)
{
    h->magic = OTA_HDR_MAGIC;
    h->hdr_crc = 0U;
    h->hdr_crc = ota_crc32((const uint8_t *)h, (uint32_t)sizeof(*h));

    /* 头独占 W25Q64 的扇区 0，整扇区擦了再写，不用「读-改-写」 */
    if (!w25q64_sector_erase(W25Q64_OTA_HDR_ADDR)) {
        return false;
    }
    return w25q64_write(W25Q64_OTA_HDR_ADDR, h, (uint32_t)sizeof(*h));
}

static bool flash_init_once(void)
{
    if (!s_flash_ok) {
        s_flash_ok = w25q64_init();
    }
    return s_flash_ok;
}

/* ==========================================================================
 * 接收：暂存 W25Q64
 * ========================================================================== */

/* 把暂存区落盘。调用时机是「刚回完 ACK、对端正在准备下一片」的空档，
 * 所以这里最坏 400ms 的扇区擦除是安全的，不会把对端逼到超时重传。
 *
 * 用「已擦到哪」的水位而不是「本块擦没擦」的布尔量：分片大小不整除 4KB 时，
 * 暂存区可能只装半块就被迫落盘（见 handle_data），按整块推进地址会在
 * W25Q64 里留洞、把固件写错位。按实际写入长度推进 + 按需补擦才是对的。 */
static bool flush_staging(void)
{
    if (s_staging_len == 0U) {
        return true;
    }

    const uint32_t end = s_stage_addr + s_staging_len;

    /* 目标区间必须已经擦过：擦除是按扇区做的，所以从水位开始逐扇区补。 */
    while (s_erased_upto < end) {
        if (!w25q64_sector_erase(s_erased_upto)) {
            return false;   /* 水位不动，下次调用会重试同一个扇区 */
        }
        s_erased_upto += W25Q64_SECTOR_SIZE;
    }

    if (!w25q64_write(s_stage_addr, s_staging, s_staging_len)) {
        return false;
    }

    s_stage_addr += s_staging_len;
    s_staging_len = 0U;
    return true;
}

static void handle_start(uint16_t seq, const uint8_t *p, uint16_t len)
{
    if (s_have_start) {
        /* 重复的 OTA_START（我们的 ACK 丢了）：只补 ACK，绝不重置进度 */
        ota_proto_send_ack(seq, OTA_ACK_OK);
        return;
    }
    /* payload = u32 total_size | u16 chunk_size | u32 crc32 | u8 ver_len | ver[] */
    if (len < 11U) {
        ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
        return;
    }

    const uint32_t total = rd_u32(&p[0]);
    const uint16_t chunk = rd_u16(&p[4]);
    const uint8_t  vlen  = p[10];

    /* vlen 必须留出 1 字节给结尾的 '\0'：version[] 后面紧跟着 reserved/hdr_crc，
     * 不补零的话上报 JSON 时会一路读进 CRC 字段（OTA_VERSION_MAX=32 时正好填满）。 */
    if (vlen >= OTA_VERSION_MAX || (uint32_t)len < (11U + (uint32_t)vlen) || chunk == 0U) {
        ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
        return;
    }
    if (total == 0U || total > OTA_APP_MAX_SIZE) {
        ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
        ota_proto_status_json("error", -1, "firmware size out of range");
        return;
    }

    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.magic = OTA_HDR_MAGIC;
    s_hdr.state = OTA_HDR_STATE_RECEIVING;
    s_hdr.total_size = total;
    /* ESP32 侧 OTA_START 里的 crc32 目前恒为 0，真正的整包 CRC 在 OTA_FINISH 里给。
     * 这里先照抄，FINISH 时再用对端给的值覆盖。 */
    s_hdr.crc32 = rd_u32(&p[6]);
    s_hdr.chunk_size = chunk;
    s_hdr.version_len = vlen;
    memcpy(s_hdr.version, &p[11], vlen);

    s_expect_seq = 0U;
    s_received = 0U;
    s_crc = 0U;
    s_staging_len = 0U;
    s_flash_bad = false;
    s_stage_addr = W25Q64_OTA_FW_ADDR;
    s_erased_upto = W25Q64_OTA_FW_ADDR;
    s_finish_seen = false;
    s_have_start = true;
    s_last_activity_ms = HAL_GetTick();

    /* ★ 先回 ACK 再做慢活。写头要擦 W25Q64 扇区 0（最坏 400ms），
     * 而对端等 ACK 只等 500ms，所以顺序反过来就会把它逼到重传。 */
    ota_proto_send_ack(seq, OTA_ACK_OK);

    if (!hdr_write(&s_hdr)) {
        s_flash_bad = true;
        ota_proto_status_json("error", -1, "spi flash header write failed");
    }
}

static void handle_data(uint16_t seq, const uint8_t *p, uint16_t len)
{
    if (!s_have_start || s_flash_bad || len == 0U) {
        ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
        return;
    }

    /* 序号判断必须在长度判断之前：最后一片重传时 s_received 已经等于 total_size，
     * 先判长度会把它当成「超长」拒掉，反而让对端白烧三次重传。 */
    if (seq != s_expect_seq) {
        if ((int16_t)(s_expect_seq - seq) > 0) {
            /* 序号落后 = 我们的 ACK 丢了导致对端重传：补一个 ACK，
             * 但**绝不重复写盘**（同一片写两次会破坏 NOR Flash 的内容）。 */
            ota_proto_send_ack(seq, OTA_ACK_OK);
        } else {
            /* 序号跳跃 = 有一片我们没收到。协议里没有「请重发第 N 片」的语义，
             * 只能告诉对端这个序号不对，它会重传当前片；重传也没用（序号不会回退），
             * 三次之后本片失败，整个 OTA 由对端判定失败后重来。 */
            ota_proto_send_ack(seq, OTA_ACK_OUT_OF_SEQ);
        }
        return;
    }

    if (((uint32_t)s_received + (uint32_t)len) > s_hdr.total_size) {
        /* 收到的比声明的总长还多：两边对 size 的理解不一致，直接拒 */
        ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
        return;
    }

    if ((s_staging_len + (uint32_t)len) > OTA_STAGING_SIZE) {
        /* 只有分片大小不整除 4KB 时才会走到这里：先把攒的落盘再收。
         * 这条路会牺牲一次 ACK 延迟（多算一次擦除），所以默认分片 1024 时
         * 永远走不到；flush_staging() 按实际长度推进地址，不会写错位。 */
        if (!flush_staging()) {
            s_flash_bad = true;
            ota_proto_send_ack(seq, OTA_ACK_FLASH_ERR);
            return;
        }
    }

    memcpy(&s_staging[s_staging_len], p, len);
    s_staging_len += len;
    s_crc = ota_crc32_update(s_crc, p, len);
    s_received += len;
    s_expect_seq = (uint16_t)(seq + 1U);
    s_last_activity_ms = HAL_GetTick();

    /* ★ 先 ACK 再落盘：擦写耗时（最坏 400ms）藏进「对端准备下一片」的空档，
     * 保证 ACK 延迟只有几百微秒，永远撞不上对端 500ms 的超时。 */
    ota_proto_send_ack(seq, OTA_ACK_OK);

    if (s_staging_len == OTA_STAGING_SIZE) {
        if (!flush_staging()) {
            s_flash_bad = true;
        }
    }
}

static void handle_finish(uint16_t seq, const uint8_t *p, uint16_t len)
{
    uint8_t result = OTA_CRC_RESULT_FAIL;

    (void)seq;
    s_finish_seen = true;   /* 不管成败，接收阶段到此结束 */

    if (!s_have_start) {
        ota_proto_send_crc_result(result);
        return;
    }

    const uint32_t expect = (len >= 4U) ? rd_u32(p) : 0U;
    bool ready = false;

    if (s_flash_bad || len < 4U) {
        ota_proto_status_json("error", -1, "transfer aborted before finish");
    } else if (s_received != s_hdr.total_size) {
        ota_proto_status_json("error", -1, "length mismatch");
    } else if (!flush_staging()) {
        ota_proto_status_json("error", -1, "spi flash write failed");
    } else if (s_crc != expect) {
        /* 这里的 s_crc 是对**收到的字节**流式累进的，和对端算出来的是同一个东西，
         * 两边不一致就说明线上有数据被改过（或对端自己读文件就读错了）。 */
        ota_proto_status_json("error", -1, "crc32 mismatch");
    } else {
        /* 只有到这里才允许置 READY —— 这是掉电安全的支点 */
        s_hdr.crc32 = expect;
        s_hdr.state = OTA_HDR_STATE_READY;
        ready = hdr_write(&s_hdr);
    }

    if (!ready) {
        /* 没通过就作废，别让半成品留在 READY 状态被后面误当成好固件 */
        s_hdr.state = OTA_HDR_STATE_EMPTY;
        (void)hdr_write(&s_hdr);
        s_have_start = false;
    }

    /* 对端 connect_stm_ota_finish() 只认 CRC_RESULT，**不能**先回一个 ACK，
     * 否则它会拿 ACK 去唤醒等待、发现不是 CRC_RESULT 而直接判失败。 */
    ota_proto_send_crc_result(ready ? OTA_CRC_RESULT_OK : OTA_CRC_RESULT_FAIL);
}

static const char *abort_reason_str(uint8_t reason)
{
    switch (reason) {
    case OTA_ABORT_DOWNLOAD_FAIL: return "peer download failed";
    case OTA_ABORT_SIZE_MISMATCH: return "peer size mismatch";
    case OTA_ABORT_CRC_MISMATCH:  return "peer crc mismatch";
    case OTA_ABORT_TIMEOUT:       return "peer timeout";
    default:                      return "peer aborted";
    }
}

static void handle_abort(const uint8_t *p, uint16_t len)
{
    const uint8_t reason = (len >= 1U) ? p[0] : 0U;

    if (s_have_start) {
        s_hdr.state = OTA_HDR_STATE_EMPTY;
        (void)hdr_write(&s_hdr);
        s_have_start = false;
    }
    s_session_end = true;
    /* OTA_ABORT 是单向通知，对端不等应答，所以这里**不回 ACK** */
    ota_proto_status_json("aborted", -1, abort_reason_str(reason));
}

static void on_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case OTA_FRAME_OTA_START:  handle_start(seq, payload, len);  break;
    case OTA_FRAME_OTA_DATA:   handle_data(seq, payload, len);   break;
    case OTA_FRAME_OTA_FINISH: handle_finish(seq, payload, len); break;
    case OTA_FRAME_OTA_ABORT:  handle_abort(payload, len);       break;
    default:
        /* 0x81/0x82/0x10/0x11 都是我们自己发出去的方向，正常不会收到 */
        break;
    }
}

/* 等 OTA_START。期间每 OTA_READY_REPEAT_MS 重发一次 ready 状态，
 * 让对端（以及云端日志）知道 STM32 已经在等了。 */
static bool wait_for_ota_start(uint32_t wait_ms)
{
    const uint32_t start = HAL_GetTick();
    uint32_t next_ready = OTA_READY_REPEAT_MS;

    while (!s_have_start && !s_session_end) {
        ota_proto_poll();
        if (wait_ms == 0U) {
            continue;
        }
        const uint32_t elapsed = HAL_GetTick() - start;
        if (elapsed >= wait_ms) {
            return false;
        }
        if (elapsed >= next_ready) {
            next_ready += OTA_READY_REPEAT_MS;
            ota_proto_status_json("ready", 0, "waiting for ota_start");
        }
    }
    return s_have_start;
}

/* 收分片，直到 OTA_FINISH / OTA_ABORT / 链路超时 */
static bool receive_firmware(void)
{
    while (!s_finish_seen && !s_session_end) {
        ota_proto_poll();
        if ((HAL_GetTick() - s_last_activity_ms) > OTA_DATA_GAP_TIMEOUT_MS) {
            ota_proto_status_json("abort", -1, "link timeout");
            if (s_have_start) {
                s_hdr.state = OTA_HDR_STATE_EMPTY;
                (void)hdr_write(&s_hdr);
                s_have_start = false;
            }
            return false;
        }
    }
    return s_hdr.state == OTA_HDR_STATE_READY;
}

/* 一次完整的「等 START -> 收完 -> 搬运」会话。
 * 返回 true 表示固件已经装进 App 区并通过了搬运后的校验。 */
static bool download_session(uint32_t wait_ms)
{
    if (!flash_init_once()) {
        ota_proto_status_json("error", -1, "spi flash not found");
        HAL_Delay(1000U);
        return false;
    }

    s_have_start = false;
    s_finish_seen = false;
    s_session_end = false;

    ota_proto_status_json("ready", 0, "waiting for ota_start");
    if (!wait_for_ota_start(wait_ms)) {
        return false;
    }
    if (!receive_firmware()) {
        return false;
    }
    return install_firmware(&s_hdr);
}

/* ==========================================================================
 * 搬运：W25Q64 -> 内部 Flash App 区
 * ========================================================================== */

static bool app_flash_erase(uint32_t size)
{
    FLASH_EraseInitTypeDef er = {0};
    uint32_t err = 0U;
    uint32_t addr = OTA_APP_ADDR;
    const uint32_t end = OTA_APP_ADDR + size;
    bool ok = true;

    HAL_FLASH_Unlock();
    while (addr < end) {
        uint32_t sector;
        uint32_t next;

        if (addr < APP_SECTOR5_BASE) {
            sector = FLASH_SECTOR_4;
            next = APP_SECTOR5_BASE;
        } else {
            const uint32_t idx = (addr - APP_SECTOR5_BASE) / APP_SECTOR5_SIZE;
            sector = FLASH_SECTOR_5 + idx;
            next = APP_SECTOR5_BASE + (idx + 1U) * APP_SECTOR5_SIZE;
        }

        er.TypeErase = FLASH_TYPEERASE_SECTORS;
        er.VoltageRange = FLASH_VOLTAGE_RANGE_3;
        er.Sector = sector;
        er.NbSectors = 1U;

        if (HAL_FLASHEx_Erase(&er, &err) != HAL_OK) {
            ok = false;
            break;
        }
        addr = next;
    }
    HAL_FLASH_Lock();
    return ok;
}

/* App 区占用的内部 Flash 是否已全部变成 0xFF（擦干净了）不容易直接判，
 * 所以这里不做单独检查：CRC32 比对已经覆盖了「搬进去的和期望的一致」。 */

static bool install_firmware(const ota_header_t *h)
{
    if (h->total_size == 0U || h->total_size > OTA_APP_MAX_SIZE) {
        return false;
    }

    /* 1. 回读 W25Q64 里真正落盘的内容，整包算 CRC32。
     *    只有「写进去的」确实等于「发过来的」，才允许动内部 Flash。 */
    {
        uint32_t readback = 0U;
        if (!w25q64_crc32_readback(W25Q64_OTA_FW_ADDR, h->total_size, &readback) ||
            readback != h->crc32) {
            ota_proto_status_json("error", -1, "spi flash readback crc mismatch");
            return false;
        }
    }

    ota_proto_status_json("installing", 0, (const char *)h->version);

    /* 2. 擦掉 App 区用到的内部扇区 */
    if (!app_flash_erase(h->total_size)) {
        ota_proto_status_json("error", -1, "app flash erase failed");
        return false;
    }

    /* 3. 4KB 一块搬过去。复用 s_staging 当搬运缓冲——接收阶段已经结束，它是空闲的。 */
    {
        uint32_t addr = OTA_APP_ADDR;
        uint32_t left = h->total_size;
        uint32_t reported_kb = 0U;
        const uint32_t total_kb = h->total_size / 1024U;
        bool ok = true;

        HAL_FLASH_Unlock();
        while (left > 0U) {
            const uint32_t n = (left < OTA_STAGING_SIZE) ? left : OTA_STAGING_SIZE;
            /* 内部 Flash 按 32 位字编程，最后一块不足 4 字节也要凑整；
             * 多读的几字节来自 W25Q64 里已擦除的区域，是 0xFF，正好等于「不写」。 */
            const uint32_t n4 = (n + 3U) & ~3U;

            if (!w25q64_read(W25Q64_OTA_FW_ADDR + (addr - OTA_APP_ADDR), s_staging, n4)) {
                ota_proto_status_json("error", -1, "spi flash read failed");
                ok = false;
                break;
            }
            for (uint32_t i = 0U; i < n4; i += 4U) {
                uint32_t word;
                memcpy(&word, &s_staging[i], sizeof(word));
                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, (uint64_t)word) !=
                    HAL_OK) {
                    ota_proto_status_json("error", -1, "app flash program failed");
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                break;
            }

            addr += n;
            left -= n;

            /* 每 128KB（以及最后一块）上报一次进度 */
            const uint32_t done_kb = (addr - OTA_APP_ADDR) / 1024U;
            if ((done_kb - reported_kb) >= 128U || left == 0U) {
                reported_kb = done_kb;
                const int percent = (total_kb > 0U) ? (int)((done_kb * 100U) / total_kb) : 100;
                ota_proto_status_json("installing", percent, NULL);
            }
        }
        HAL_FLASH_Lock();
        if (!ok) {
            return false;
        }
    }

    /* 4. 回读内部 Flash 整包算 CRC32。搬运后必须再验一次：
     *    上一步的 CRC 只证明了 W25Q64 里是对的，没证明内部 Flash 写对了。 */
    {
        const uint32_t got =
            ota_crc32((const uint8_t *)OTA_APP_ADDR, h->total_size);
        if (got != h->crc32) {
            ota_proto_status_json("error", -1, "app flash verify failed");
            return false;
        }
    }

    /* 5. 打上 CONSUMED，这样下次上电不会再拿这份固件去搬 */
    {
        ota_header_t done = *h;
        done.state = OTA_HDR_STATE_CONSUMED;
        (void)hdr_write(&done);
    }

    ota_proto_status_json("installed", 100, (const char *)h->version);
    return true;
}

/* ==========================================================================
 * 入口
 * ========================================================================== */

static bool install_ready_firmware(void)
{
    ota_header_t h;

    if (!flash_init_once()) {
        return false;
    }
    if (!hdr_read(&h)) {
        return false;
    }
    if (h.state != OTA_HDR_STATE_READY) {
        return false;
    }
    return install_firmware(&h);
}

void ota_boot_run(void)
{
    const bool crc_ok = ota_proto_init();
    ota_proto_register_rx(on_frame);

    if (!crc_ok) {
        /* CRC 自检不过说明核心算法有问题。这时候宁可什么都不做：
         * 用错误的校验逻辑去刷 App 区，等于把「校验通过」这四个字作废。 */
        ota_proto_status_json("error", -1, "crc selftest failed");
        for (;;) {
            ota_proto_poll();
        }
    }

    const bool requested = boot_requested();

    if (!requested) {
        /* 正常上电的两条快路径，都几乎零延迟：
         *   a. 上次收完却没能搬完（中途掉电）-> 补搬一次
         *   b. App 是好的 -> 直接跑 */
        if (install_ready_firmware()) {
            jump_to_app();
        }
        if (app_valid()) {
            jump_to_app();
        }
    }

    /* 到这里说明：要么 App 明确要求升级（写了 BKP0R 魔数后复位），
     * 要么 App 区是空的/坏的。进下载模式。 */
    ota_proto_status_json(requested ? "boot" : "recover", 0,
                          requested ? "download mode requested"
                                    : "app invalid, waiting for ota");

    for (;;) {
        if (install_ready_firmware()) {
            jump_to_app();
        }
        if (download_session(OTA_WAIT_START_MS)) {
            jump_to_app();
        }
        if (app_valid()) {
            /* 这一轮没升级成功，但 App 还在（比如对端一直没来）——
             * 让 App 先跑起来，比卡在 Bootloader 里强。 */
            jump_to_app();
        }
        /* 都不行就再等一轮 OTA_START */
    }
}
