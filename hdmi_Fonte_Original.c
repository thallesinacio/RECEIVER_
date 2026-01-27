#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h" 
#include "hardware/uart.h" 
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/dma.h"


#include "hardware/clocks.h"          // Para set_sys_clock_khz
#include "hardware/structs/bus_ctrl.h" // Para bus_ctrl_hw e prioridades
// ------------------------------------------

// --- INCLUDES DO DVI (PicoDVI) ---
#include "dvi.h"
#include "dvi_serialiser.h"
#include "./include/common_dvi_pin_configs.h"
#include "tmds_encode_font_2bpp.h"
#include "./assets/font_teste.h"

// --- CONFIGURAÇÕES DA UART (RECEIVER) ---
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1

// --- CONFIGURAÇÕES DE VÍDEO ---
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 24         
#define FONT_ORIGINAL_HEIGHT 8      
#define FONT_SCALE_FACTOR (FONT_CHAR_HEIGHT / FONT_ORIGINAL_HEIGHT) 

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

#define CHAR_COLS (FRAME_WIDTH / FONT_CHAR_WIDTH)
#define CHAR_ROWS (FRAME_HEIGHT / FONT_CHAR_HEIGHT) 
#define COLOUR_PLANE_SIZE_WORDS (CHAR_ROWS * CHAR_COLS * 4 / 32)

// --- VARIÁVEIS GLOBAIS DE DADOS ---
// Armazenam os últimos valores recebidos para exibição
float g_bmp_temp = 0.0f;
float g_bmp_alt = 0.0f;
float g_aht_temp = 0.0f;
float g_aht_hum = 0.0f;
char uart_buffer[128]; // Buffer para acumular a linha da UART
int uart_pos = 0;

// Objetos DVI
struct dvi_inst dvi0;
char charbuf[CHAR_ROWS * CHAR_COLS];
uint32_t colourbuf[3 * COLOUR_PLANE_SIZE_WORDS];

// Botão Boot
#define botaoB 6
void gpio_irq_handler(uint gpio, uint32_t events) {
    reset_usb_boot(0, 0);
}

// --- FUNÇÕES DE DESENHO ---

static inline void set_char(uint x, uint y, char c) {
    if (x >= CHAR_COLS || y >= CHAR_ROWS) return;
    charbuf[x + y * CHAR_COLS] = c;
}

static inline void set_colour(uint x, uint y, uint8_t fg, uint8_t bg) {
    if (x >= CHAR_COLS || y >= CHAR_ROWS) return;
    uint char_index = x + y * CHAR_COLS;
    uint bit_index = char_index % 8 * 4;
    uint word_index = char_index / 8;
    for (int plane = 0; plane < 3; ++plane) {
        uint32_t fg_bg_combined = (fg & 0x3) | (bg << 2 & 0xc);
        colourbuf[word_index] = (colourbuf[word_index] & ~(0xfu << bit_index)) | (fg_bg_combined << bit_index);
        fg >>= 2;
        bg >>= 2;
        word_index += COLOUR_PLANE_SIZE_WORDS;
    }
}

// Função auxiliar para escrever strings na tela
void draw_text(int x, int y, const char* text, uint8_t fg, uint8_t bg) {
    int len = strlen(text);
    for (int i = 0; i < len; ++i) {
        set_char(x + i, y, text[i]);
        set_colour(x + i, y, fg, bg);
    }
}

// Limpa uma linha específica (para atualizar valores sem piscar a tela toda)
void clear_line_area(int x, int y, int length, uint8_t bg) {
    for (int i = 0; i < length; ++i) {
        set_char(x + i, y, ' ');
        set_colour(x + i, y, 0x00, bg);
    }
}

void draw_border() {
    const uint8_t fg = 0x15; 
    const uint8_t bg = 0x00; // Fundo preto para ficar mais limpo
    
    // Desenha bordas simples
    for (uint x = 0; x < CHAR_COLS; ++x) {
        set_char(x, 0, '='); set_colour(x, 0, fg, bg);
        set_char(x, CHAR_ROWS - 1, '='); set_colour(x, CHAR_ROWS - 1, fg, bg);
    }
    for (uint y = 0; y < CHAR_ROWS; ++y) {
        set_char(0, y, '|'); set_colour(0, y, fg, bg);
        set_char(CHAR_COLS - 1, y, '|'); set_colour(CHAR_COLS - 1, y, fg, bg);
    }
}

// --- FUNÇÃO DE PROCESSAMENTO DE DADOS ---
void parse_uart_data(char *line) {
    float t, v2; // v2 será altitude ou umidade

    // Tenta ler o formato exato do BMP
    // Procura por "TB:" (Temp BMP) e "AL:" (Altitude)
    if (sscanf(line, "SENSOR:BMP280,TB:%f,AL:%f", &t, &v2) == 2) {
        g_bmp_temp = t;
        g_bmp_alt = v2;
    } 
    // Tenta ler o formato exato do AHT
    // Procura por "TA:" (Temp AHT) e "UM:" (Umidade)
    else if (sscanf(line, "SENSOR:AHT20,TA:%f,UM:%f", &t, &v2) == 2) {
        g_aht_temp = t;
        g_aht_hum = v2;
    }
}

// --- CORE 1: VÍDEO ---
void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            uint font_row = (y % FONT_CHAR_HEIGHT) / FONT_SCALE_FACTOR; 
            uint32_t *tmdsbuf;
            queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
            for (int plane = 0; plane < 3; ++plane) {
                tmds_encode_font_2bpp(
                    (const uint8_t*)&charbuf[y / FONT_CHAR_HEIGHT * CHAR_COLS],
                    &colourbuf[y / FONT_CHAR_HEIGHT * (COLOUR_PLANE_SIZE_WORDS / CHAR_ROWS) + plane * COLOUR_PLANE_SIZE_WORDS],
                    tmdsbuf + plane * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD),
                    FRAME_WIDTH,
                    (const uint8_t*)&font_8x8[font_row * FONT_N_CHARS] - FONT_FIRST_ASCII
                );
            }
            queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

// --- CORE 0: LÓGICA PRINCIPAL ---
int __not_in_flash("main") main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    
    // 1. Configura Clock para DVI
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // 2. Configura UART 
    // O Sender envia no pino TX(GP0) -> Devemos ligar no RX(GP1) deste Pico
    // Mas a inicialização padrão usa uart0
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Botão de boot
    gpio_init(botaoB);
    gpio_set_dir(botaoB, GPIO_IN);
    gpio_pull_up(botaoB);
    gpio_set_irq_enabled_with_callback(botaoB, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    // 3. Inicializa DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = picodvi_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Limpa tela
    for (uint y = 0; y < CHAR_ROWS; ++y) {
        for (uint x = 0; x < CHAR_COLS; ++x) {
            set_char(x, y, ' ');
            set_colour(x, y, 0x00, 0x00);
        }
    }
    draw_border();

    // Textos Estáticos (Layout)
    draw_text(25, 2, "MONITORAMENTO DE SENSORES", 0x3f, 0x00); // Branco
    draw_text(30, 3, "(Recebimento via UART)", 0x15, 0x00);   // Cinza

    // Labels
    draw_text(10, 6, "SENSOR BMP280 (Pressao):", 0x33, 0x00); // Ciano
    draw_text(10, 8, "  Temperatura:", 0x3f, 0x00);
    draw_text(10, 9, "  Altitude...:", 0x3f, 0x00);

    draw_text(10, 12, "SENSOR AHT20  (Umidade):", 0x30, 0x00); // Amarelo
    draw_text(10, 14, "  Temperatura:", 0x3f, 0x00);
    draw_text(10, 15, "  Umidade....:", 0x3f, 0x00);

    // Inicia Core 1
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);

    char val_str[32]; // Buffer auxiliar para converter float em string

    while (true) {
        // --- 1. LER UART (Sem bloquear o vídeo) ---
        while (uart_is_readable(UART_ID)) {
            char c = uart_getc(UART_ID);
            
            if (c == '\n' || c == '\r') {
                if (uart_pos > 0) {
                    uart_buffer[uart_pos] = 0; // Finaliza string
                    parse_uart_data(uart_buffer); // Processa
                    uart_pos = 0; // Reseta buffer
                }
            } else {
                if (uart_pos < 127) {
                    uart_buffer[uart_pos++] = c;
                }
            }
        }

        // --- 2. ATUALIZAR TELA ---
        // Apenas atualiza os valores numéricos
        
        // BMP Temp
        sprintf(val_str, "%.2f C", g_bmp_temp);
        draw_text(30, 8, val_str, 0x03, 0x00); // Vermelho se quente, ou verde? Usando Verde (0x0C) ou outro
        set_colour(30, 8, 0x3F, 0x00); // Branco para o valor

        // BMP Alt
        sprintf(val_str, "%.2f m", g_bmp_alt);
        clear_line_area(30, 9, 10, 0x00); // Limpa resíduo anterior
        draw_text(30, 9, val_str, 0x3F, 0x00);

        // AHT Temp
        sprintf(val_str, "%.2f C", g_aht_temp);
        clear_line_area(30, 14, 10, 0x00);
        draw_text(30, 14, val_str, 0x3F, 0x00);

        // AHT Hum
        sprintf(val_str, "%.2f %%", g_aht_hum);
        clear_line_area(30, 15, 10, 0x00);
        draw_text(30, 15, val_str, 0x3F, 0x00);

        // Pequeno sleep para não saturar a CPU 0 à toa, mas curto o suficiente para a UART
        sleep_ms(10);
    }
}