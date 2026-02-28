#ifndef EPDIF_H
#define EPDIF_H

#include <Arduino.h>

// XIAO ESP32-S3 pinout
#define RST_PIN          4  // D3 -> GPIO4
#define DC_PIN           3  // D2 -> GPIO3
#define CS_PIN           2  // D1 -> GPIO2
#define BUSY_PIN         5  // D4 -> GPIO5
#define CLK_PIN          7  // D8 -> GPIO7 (SCK)
#define DIN_PIN          9  // D10 -> GPIO9 (MOSI)

class EpdIf {
public:
    EpdIf(void);
    ~EpdIf(void);

    static int  IfInit(void);
    static void DigitalWrite(int pin, int value);
    static int  DigitalRead(int pin);
    static void DelayMs(unsigned int delaytime);
    static void SpiTransfer(unsigned char data);
    static void SpiTransferBulk(unsigned char* data, int len);
};

#endif
