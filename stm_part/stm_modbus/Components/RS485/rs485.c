#include "rs485.h"

/*----------------------------------------------- 内部状态 -------------------------------------------*/

static UART_HandleTypeDef s_huart;

/* 环形接收缓冲：中断里写 head，主循环里读 tail */
static volatile uint8_t  s_rx[RS485_RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

/* 分帧用：收到过字节就置位，同时记下最后一个字节的时间戳 */
static volatile uint8_t  s_rx_active;
static volatile uint32_t s_rx_last_tick;

/*----------------------------------------------- 方向控制 -------------------------------------------*/

#if RS485_DE_ENABLE

static void RS485_SetTx(void)
{
    HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_SET);
}

static void RS485_SetRx(void)
{
    HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_RESET);
}

#else

/* 自动换向模块：没有 DE/RE，收发方向由模块自己按 TX 数据切换 */
static inline void RS485_SetTx(void) {}
static inline void RS485_SetRx(void) {}

#endif

/*----------------------------------------------- 初始化 -------------------------------------------*/

void RS485_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    RS485_UART_CLK_ENABLE();
    RS485_GPIO_CLK_ENABLE();

    /* PA2 = USART2_TX，PA3 = USART2_RX */
    GPIO_InitStruct.Pin       = RS485_TX_PIN | RS485_RX_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = RS485_GPIO_AF;
    HAL_GPIO_Init(RS485_GPIO_PORT, &GPIO_InitStruct);

#if RS485_DE_ENABLE
    RS485_DE_CLK_ENABLE();

    /* DE/RE，默认接收 */
    GPIO_InitStruct.Pin       = RS485_DE_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = 0;
    HAL_GPIO_Init(RS485_DE_PORT, &GPIO_InitStruct);
    RS485_SetRx();
#endif

    /* 注意：HAL_UART_Init 会调 HAL_UART_MspInit（CubeMX 那份，只处理 USART1），
     * 对 USART2 什么都不做，所以上面的时钟和 GPIO 必须自己配。 */
    s_huart.Instance          = RS485_UART;
    s_huart.Init.BaudRate     = baudrate;
    s_huart.Init.WordLength   = UART_WORDLENGTH_8B;
    s_huart.Init.StopBits     = UART_STOPBITS_1;
    s_huart.Init.Parity       = UART_PARITY_NONE;
    s_huart.Init.Mode         = UART_MODE_TX_RX;
    s_huart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    s_huart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart) != HAL_OK)
    {
        Error_Handler();
    }

    s_rx_head      = 0;
    s_rx_tail      = 0;
    s_rx_active    = 0;
    s_rx_last_tick = 0;

    __HAL_UART_ENABLE_IT(&s_huart, UART_IT_RXNE);
    HAL_NVIC_SetPriority(RS485_UART_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(RS485_UART_IRQn);
}

/*----------------------------------------------- 接收中断 -------------------------------------------*/

void USART2_IRQHandler(void)
{
    uint32_t sr = RS485_UART->SR;

    if (sr & USART_SR_RXNE)
    {
        uint8_t  b    = (uint8_t)(RS485_UART->DR & 0xFFU);
        uint16_t next = (uint16_t)((s_rx_head + 1U) % RS485_RX_RING_SIZE);

        if (next != s_rx_tail)      /* 满了就丢这个字节，等上层超时重来 */
        {
            s_rx[s_rx_head] = b;
            s_rx_head       = next;
        }

        s_rx_active    = 1U;
        s_rx_last_tick = HAL_GetTick();
    }
    else if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
    {
        (void)RS485_UART->DR;       /* 先读 SR 再读 DR 才能清掉这些错误标志 */
    }
}

/*----------------------------------------------- 发送 -------------------------------------------*/

HAL_StatusTypeDef RS485_Send(const uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef st;

    RS485_SetTx();

#if RS485_DE_ENABLE
    /* 等收发器把驱动打开（MAX485 一类的使能时间在百 ns 量级，这里给约 1us） */
    for (volatile uint32_t d = 0; d < 200U; d++)
    {
        __NOP();
    }
#endif

    st = HAL_UART_Transmit(&s_huart, (uint8_t *)buf, len, RS485_TX_TIMEOUT_MS);

    /* 等最后一个 bit 真的移出去。
     * DE_ENABLE=1 时这是必须的：松 DE 早一个 bit 尾字节就会被截断；
     * 自动换向模块下也让总线先空下来，再去听回包，逻辑更干净。 */
    while ((RS485_UART->SR & USART_SR_TC) == 0U)
    {
    }

    RS485_SetRx();

    /* 把发送期间收到的东西全丢掉。
     * 如果板上 DE/RE 是各自独立的、RE 一直拉着（接收常开），我们自己发出去的那几个字节
     * 会被收发器原样收回来塞进环形缓冲，后面就会「先解析到自己的请求」而永远对不上。
     * 收完 TC 再清一次缓冲最省事；从站这时还没开始回（它得先把请求收全），不会误伤。
     * 若板上 DE/RE 是并在一起接同一个 GPIO 的，这里就是个空操作。 */
    RS485_FlushRx();

    return st;
}

/*----------------------------------------------- 接收缓冲操作 -------------------------------------------*/

void RS485_FlushRx(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    s_rx_tail   = s_rx_head;
    s_rx_active = 0U;
    __set_PRIMASK(primask);
}

uint16_t RS485_GetFrame(uint8_t *out, uint16_t max)
{
    uint16_t n = 0;
    uint32_t primask;

    if (s_rx_active == 0U)
    {
        return 0;
    }

    /* 静默时间还不够，继续等 */
    if ((HAL_GetTick() - s_rx_last_tick) < RS485_FRAME_GAP_MS)
    {
        return 0;
    }

    /* 关中断把整帧一次性搬出来，免得搬到一半又被中断塞进新字节 */
    primask = __get_PRIMASK();
    __disable_irq();

    s_rx_active = 0U;

    while ((s_rx_tail != s_rx_head) && (n < max))
    {
        out[n++] = s_rx[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) % RS485_RX_RING_SIZE);
    }
    s_rx_tail = s_rx_head;          /* 超长的部分直接丢弃 */

    __set_PRIMASK(primask);

    return n;
}
