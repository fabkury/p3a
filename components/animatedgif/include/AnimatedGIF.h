// Copyright 2020 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================

#ifndef __ANIMATEDGIF__
#define __ANIMATEDGIF__

#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <cstring>
#else
#include <string.h>
#endif

//
// GIF Animator
// Written by Larry Bank
// Copyright (c) 2020 BitBank Software, Inc.
// bitbank@pobox.com
//

/* GIF Defines and variables */
#define MAX_CHUNK_SIZE 255
#define TURBO_BUFFER_SIZE 0x6100
#define MAX_CODE_SIZE 12
#define MAX_COLORS 256
#define MAX_WIDTH 480
#define LZW_BUF_SIZE (6*MAX_CHUNK_SIZE)
#define LZW_HIGHWATER (4*MAX_CHUNK_SIZE)
#define LZW_BUF_SIZE_TURBO (TURBO_BUFFER_SIZE)
#define LZW_HIGHWATER_TURBO (TURBO_BUFFER_SIZE - 0x1000)
#define FILE_BUF_SIZE (1<<MAX_CODE_SIZE)
#define REGISTER_WIDTH 32
#define BIGUINT uint32_t

#define PIXEL_FIRST 0
#define PIXEL_LAST (1<<MAX_CODE_SIZE)
#define LINK_UNUSED 5911
#define LINK_END 5912
#define MAX_HASH 5003

//
// Pixel types
//
enum {
   GIF_PALETTE_RGB565_LE = 0, // little endian (default)
   GIF_PALETTE_RGB565_BE,     // big endian
   GIF_PALETTE_RGB888,        // original 24-bpp entries
   GIF_PALETTE_RGB8888,       // 32-bit (alpha = 0xff or 0x00)
   GIF_PALETTE_1BPP,          // 1-bit per pixel (horizontal, MSB on left)
   GIF_PALETTE_1BPP_OLED      // 1-bit per pixel (vertical, LSB on top)
};

//
// Draw types
//
enum {
   GIF_DRAW_RAW = 0,
   GIF_DRAW_COOKED
};

enum {
   GIF_SUCCESS = 0,
   GIF_DECODE_ERROR,
   GIF_TOO_WIDE,
   GIF_INVALID_PARAMETER,
   GIF_UNSUPPORTED_FEATURE,
   GIF_FILE_NOT_OPEN,
   GIF_EARLY_EOF,
   GIF_EMPTY_FRAME,
   GIF_BAD_FILE,
   GIF_ERROR_MEMORY
};

typedef struct gif_file_tag
{
  int32_t iPos; // current file position
  int32_t iSize; // file size
  uint8_t *pData; // memory file pointer
  void *fHandle; // FILE* handle for file-based access
} GIFFILE;

typedef struct gif_info_tag
{
  int32_t iFrameCount; // total frames in file
  int32_t iDuration; // duration of animation in milliseconds
  int32_t iMaxDelay; // maximum frame delay
  int32_t iMinDelay; // minimum frame delay
} GIFINFO;

typedef struct gif_draw_tag
{
    int iX, iY; // Corner offset of this frame on the canvas
    int y; // current line being drawn (0 = top line of image)
    int iWidth, iHeight; // size of this frame
    int iCanvasWidth; // need this to know where to place output in a fully cooked bitmap
    void *pUser; // user supplied pointer
    uint8_t *pPixels; // 8-bit source pixels for this line
    uint16_t *pPalette; // little or big-endian RGB565 palette entries (default)
    uint8_t *pPalette24; // RGB888 palette (optional)
    uint8_t ucTransparent; // transparent color
    uint8_t ucHasTransparency; // flag indicating the transparent color is in use
    uint8_t ucDisposalMethod; // frame disposal method
    uint8_t ucBackground; // background color
    uint8_t ucPaletteType; // type of palette entries
    uint8_t ucIsGlobalPalette; // Flag to indicate that a global palette, rather than a local palette is being used
} GIFDRAW;

// Callback function prototypes
typedef int32_t (*GIF_READ_CALLBACK)(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef int32_t (*GIF_SEEK_CALLBACK)(GIFFILE *pFile, int32_t iPosition);
typedef void (*GIF_DRAW_CALLBACK)(GIFDRAW *pDraw);
typedef void * (*GIF_OPEN_CALLBACK)(const char *szFilename, int32_t *pFileSize);
typedef void (*GIF_CLOSE_CALLBACK)(void *pHandle);
typedef void * (*GIF_ALLOC_CALLBACK)(uint32_t iSize);
typedef void (*GIF_FREE_CALLBACK)(void *buffer);

//
// our private structure to hold a GIF image decode state
//
typedef struct gif_image_tag
{
    uint16_t iWidth, iHeight, iCanvasWidth, iCanvasHeight;
    uint16_t iX, iY; // GIF corner offset
    uint16_t iBpp;
    int16_t iError; // last error
    uint16_t iFrameDelay; // delay in milliseconds for this frame
    int16_t iRepeatCount; // NETSCAPE animation repeat count. 0=forever
    uint16_t iXCount, iYCount; // decoding position in image (countdown values)
    int iLZWOff; // current LZW data offset
    int iLZWSize; // current quantity of data in the LZW buffer
    int iCommentPos; // file offset of start of comment data
    short sCommentLen; // length of comment
    unsigned char bEndOfFrame;
    unsigned char ucGIFBits, ucBackground, ucTransparent, ucCodeStart, ucMap, bUseLocalPalette;
    unsigned char ucPaletteType; // RGB565 or RGB888
    unsigned char ucDrawType; // RAW or COOKED
    GIF_READ_CALLBACK pfnRead;
    GIF_SEEK_CALLBACK pfnSeek;
    GIF_DRAW_CALLBACK pfnDraw;
    GIF_OPEN_CALLBACK pfnOpen;
    GIF_CLOSE_CALLBACK pfnClose;
    GIFFILE GIFFile;
    void *pUser;
    unsigned char *pFrameBuffer;
    unsigned char *pTurboBuffer;
    unsigned char *pPixels, *pOldPixels;
    unsigned char ucFileBuf[FILE_BUF_SIZE]; // holds temp data and pixel stack
    unsigned short pPalette[(MAX_COLORS * 3)/2]; // can hold RGB565 or RGB888 - set in begin()
    unsigned short pLocalPalette[(MAX_COLORS * 3)/2]; // color palettes for GIF images
    unsigned char ucLZW[LZW_BUF_SIZE]; // holds de-chunked LZW data
    // These next 3 are used in Turbo mode to have a larger ucLZW buffer
    unsigned short usGIFTable[1<<MAX_CODE_SIZE];
    unsigned char ucGIFPixels[(PIXEL_LAST*2)];
    unsigned char ucLineBuf[MAX_WIDTH]; // current line
} GIFIMAGE;

#ifdef __cplusplus
//
// The GIF class wraps portable C code which does the actual work
//
class AnimatedGIF
{
  private:
    GIFIMAGE _gif;
    static int32_t readMem(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
    static int32_t seekMem(GIFFILE *pFile, int32_t iPosition);
#ifdef ARDUINO
    static int32_t readFLASH(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
#endif

  public:
    AnimatedGIF();
    void begin(unsigned char ucPaletteType);
    int open(const char *szFilename, GIF_OPEN_CALLBACK pfnOpen, GIF_CLOSE_CALLBACK pfnClose, GIF_READ_CALLBACK pfnRead, GIF_SEEK_CALLBACK pfnSeek, GIF_DRAW_CALLBACK pfnDraw);
    int open(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK pfnDraw);
    void close();
    void reset();
    int playFrame(bool bSync, int *delayMilliseconds, void *pUser);
    int getCanvasWidth();
    int getCanvasHeight();
    int getLoopCount();
    int getFrameWidth();
    int getFrameHeight();
    int getFrameXOff();
    int getFrameYOff();
    int setDrawType(int iType);
    int getInfo(GIFINFO *pInfo);
    int getLastError();
    int getComment(char *pDest);
    int allocFrameBuf(GIF_ALLOC_CALLBACK pfnAlloc);
    int allocTurboBuf(GIF_ALLOC_CALLBACK pfnAlloc);
    void setFrameBuf(void *pFrameBuf);
    void setTurboBuf(void *pBuf);
    uint8_t *getFrameBuf();
    uint8_t *getTurboBuf();
    int freeFrameBuf(GIF_FREE_CALLBACK pfnFree);
    int freeTurboBuf(GIF_FREE_CALLBACK pfnFree);
#ifdef ARDUINO
    int openFLASH(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK pfnDraw);
#endif
};
#endif // __cplusplus

#define INTELSHORT(p) ((*p) + (*(p+1)<<8))
#define INTELLONG(p) ((*p) + (*(p+1)<<8) + (*(p+2)<<16) + (*(p+3)<<24))

#endif // __ANIMATEDGIF__
