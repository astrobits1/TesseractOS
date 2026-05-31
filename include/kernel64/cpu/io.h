#ifndef CPU_IO_H
#define CPU_IO_H
#include <stdint.h>

/* out_u8/out_u16/out_u32 - Write byte/word/long to 16 bit IO port */
/* in_u8/in_u16/in_u32 - Read byte/word/long from 16 bit IO port and return */

inline void out_u8(uint8_t u8_, uint16_t port) {
    __asm__ volatile("outb %b0, %w1" : : "a"(u8_), "Nd"(port) : "memory");
}

inline void out_u16(uint16_t u16_, uint16_t port) {
    __asm__ volatile("outw %w0, %w1" : : "a"(u16_), "Nd"(port) : "memory");
}

inline void out_u32(uint32_t u32_, uint16_t port) {
    __asm__ volatile("outw %w0, %w1" : : "a"(u32_), "Nd"(port) : "memory");
}

inline uint8_t in_u8(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

inline uint16_t in_u16(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inb %w1, %w0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

inline uint32_t in_u32(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inb %w1, %w0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

#endif
