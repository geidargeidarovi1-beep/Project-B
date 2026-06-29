#ifndef GEORGIAN_FONT_H
#define GEORGIAN_FONT_H

#include <U8g2_for_Adafruit_GFX.h>

// Eğer orijinal özel font dosyanız kayıpsa, 
// U8g2 kütüphanesinin kendi içindeki standart Gürcüce destekli fontunu kullanıyoruz.
extern const uint8_t u8g2_font_unifont_t_georgian[]; 

#define my_georgian_font u8g2_font_unifont_t_georgian

#endif
