#include <stdint.h>
// Export byte atomics required by this image's libisp.so. Use GCC's libatomic.
uint8_t compat_add(volatile uint8_t *p, uint8_t v) __asm__("__sync_fetch_and_add_1");
uint8_t compat_and(volatile uint8_t *p, uint8_t v) __asm__("__sync_fetch_and_and_1");
uint8_t compat_add(volatile uint8_t *p,uint8_t v){return __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST);}
uint8_t compat_and(volatile uint8_t *p,uint8_t v){return __atomic_fetch_and(p,v,__ATOMIC_SEQ_CST);}
