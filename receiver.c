#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

// Mantivemos a UART0 no receptor (Pinos 0 e 1) - Sem problemas aqui
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1

int main() {
    stdio_init_all();

    // Espera USB conectar
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    printf("\n\n=== RECEPTOR PRONTO (Conecte Sender TX GP4 -> RX GP1) ===\n");

    while (true) {
        // Se houver dados chegando no fio RX
        if (uart_is_readable(UART_ID)) {
            char c = uart_getc(UART_ID);
            
            // MUDANÇA: Apenas imprime o caractere direto.
            // O Sender já está enviando a formatação correta (\r\n).
            printf("%c", c);
        }
    }
}