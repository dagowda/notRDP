/*
 * beacon.h - Cobalt Strike / Havoc compatible BOF API header
 *
 * Author:  Dhanush Gowda
 * Project: havoc-screenshare
 *
 * Standard Beacon Object File API used by both Cobalt Strike and Havoc C2.
 * Provides data parsing, formatted output, and token manipulation primitives
 * that the CoffeeLdr (Havoc) or beacon loader (CS) resolves at runtime.
 */
#pragma once
#include <windows.h>

/* Output type constants */
#define CALLBACK_OUTPUT      0x0
#define CALLBACK_OUTPUT_OEM  0x1e
#define CALLBACK_ERROR       0x0d
#define CALLBACK_OUTPUT_UTF8 0x20

/* Data parser - for reading packed BOF arguments */
typedef struct {
    char *original;
    char *buffer;
    int   length;
    int   size;
} datap;

/* Format buffer - for building structured output */
typedef struct {
    char *original;
    char *buffer;
    int   length;
    int   size;
} formatp;

/* Data API */
DECLSPEC_IMPORT void  BeaconDataParse(datap *parser, char *buffer, int size);
DECLSPEC_IMPORT int   BeaconDataInt(datap *parser);
DECLSPEC_IMPORT short BeaconDataShort(datap *parser);
DECLSPEC_IMPORT int   BeaconDataLength(datap *parser);
DECLSPEC_IMPORT char *BeaconDataExtract(datap *parser, int *size);

/* Format API */
DECLSPEC_IMPORT void  BeaconFormatAlloc(formatp *format, int maxsz);
DECLSPEC_IMPORT void  BeaconFormatReset(formatp *format);
DECLSPEC_IMPORT void  BeaconFormatFree(formatp *format);
DECLSPEC_IMPORT void  BeaconFormatAppend(formatp *format, char *text, int len);
DECLSPEC_IMPORT void  BeaconFormatPrintf(formatp *format, char *fmt, ...);
DECLSPEC_IMPORT char *BeaconFormatToString(formatp *format, int *size);
DECLSPEC_IMPORT void  BeaconFormatInt(formatp *format, int value);

/* Output API */
DECLSPEC_IMPORT void  BeaconPrintf(int type, char *fmt, ...);
DECLSPEC_IMPORT void  BeaconOutput(int type, char *data, int len);

/* Token API */
DECLSPEC_IMPORT BOOL  BeaconUseToken(HANDLE token);
DECLSPEC_IMPORT void  BeaconRevertToken(void);
DECLSPEC_IMPORT BOOL  BeaconIsAdmin(void);
