#ifndef PLATFORM_INFO_H
#define PLATFORM_INFO_H

#include <stdint.h>
#define IS_64_BIT (UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFU)
#define IS_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#ifdef TARGET_VITA
// The Vita is a 32-bit machine, but the sound bank data keeps its 64-bit layout there
// (see AUDIO_FILE_PTR in src/audio/internal.h). The audio heap and its bank/sequence pools
// therefore have to be as large as on a 64-bit machine, or the bigger banks do not fit
// and whole levels go silent.
#define DOUBLE_SIZE_ON_64_BIT(size) ((size) * 2)
#else
#define DOUBLE_SIZE_ON_64_BIT(size) ((size) * (sizeof(void *) / 4))
#endif

#endif // PLATFORM_INFO_H
