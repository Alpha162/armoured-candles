#ifndef EPD7IN5_V2_H
#define EPD7IN5_V2_H

#include "epdif.h"

#define EPD_WIDTH       800
#define EPD_HEIGHT      480

class Epd : EpdIf {
public:
    Epd();
    ~Epd();
    int  Init(void);
    void WaitUntilIdle(void);
    void Reset(void);
    void DisplayFrame(const unsigned char* frame_buffer);
    void DisplayFramePartial(const unsigned char* old_buf, const unsigned char* new_buf);
    void SendCommand(unsigned char command);
    void SendData(unsigned char data);
    void Sleep(void);
    void Clear(void);

private:
    unsigned int reset_pin;
    unsigned int dc_pin;
    unsigned int cs_pin;
    unsigned int busy_pin;
    unsigned long width;
    unsigned long height;
    void SetLut_by_host(unsigned char* lut_vcom, unsigned char* lut_ww,
                        unsigned char* lut_bw, unsigned char* lut_wb, unsigned char* lut_bb);
};

#endif
