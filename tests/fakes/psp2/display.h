#pragma once
typedef struct SceDisplayFrameBuf { void *base; } SceDisplayFrameBuf;
typedef enum SceDisplaySetBufSync { SCE_DISPLAY_SETBUF_IMMEDIATE = 0, SCE_DISPLAY_SETBUF_NEXTFRAME = 1 } SceDisplaySetBufSync;
