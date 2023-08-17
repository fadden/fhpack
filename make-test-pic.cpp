/*
 * Generate some 8K images for fhpack testing.
 * By Andy McFadden
 * Version 1.0, August 2015
 *
 * Copyright 2015 by faddenSoft.  All Rights Reserved.
 * See the LICENSE.txt file for distribution terms (Apache 2.0).
 */
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>

const char* TEST_ALL_ZERO = "allzero#060000";
const char* TEST_ALL_GREEN = "allgreen#060000";
const char* TEST_NO_MATCH = "nomatch#060000";
const char* TEST_HALF_HALF = "halfhalf#060000";

// Generates a pattern that foils the matcher.
static void WriteNoMatch(FILE* fp)
{
    for (int ic = 0; ic < 252; ic++) {
        putc(ic, fp);
        putc(ic+1, fp);
        putc(ic+2, fp);
        putc(ic+3, fp);
    }
    // 1008
    for (int ic = 0; ic < 252; ic++) {
        putc(ic, fp);
        putc(ic+2, fp);
        putc(ic+1, fp);
        putc(ic+3, fp);
    }
    // 2016
    for (int ic = 0; ic < 252; ic++) {
        putc(ic, fp);
        putc(ic+1, fp);
        putc(ic+3, fp);
        putc(ic+2, fp);
    }
    // 3024
    for (int ic = 0; ic < 252; ic++) {
        putc(ic, fp);
        putc(ic+3, fp);
        putc(ic+2, fp);
        putc(ic+1, fp);
    }
    // 4032
    for (int ic = 0; ic < 252; ic++) {
        putc(ic, fp);
        putc(ic+3, fp);
        putc(ic+1, fp);
        putc(ic+2, fp);
    }
    // 5040
    for (int ic = 0; ic < 252; ic++) {
        putc(ic+1, fp);
        putc(ic, fp);
        putc(ic+2, fp);
        putc(ic+3, fp);
    }
    // 6048
    for (int ic = 0; ic < 252; ic++) {
        putc(ic+1, fp);
        putc(ic+2, fp);
        putc(ic, fp);
        putc(ic+3, fp);
    }
    // 7056
    for (int ic = 0; ic < 252; ic++) {
        putc(ic+1, fp);
        putc(ic+2, fp);
        putc(ic+3, fp);
        putc(ic, fp);
    }
    // 8064
    for (int ic = 0; ic < 32; ic++) {
        putc(ic+2, fp);
        putc(ic+1, fp);
        putc(ic+3, fp);
        putc(ic, fp);
    }
}

int main()
{
    FILE* fp;

    if (access(TEST_ALL_ZERO, F_OK) == 0) {
        printf("NOT overwriting %s\n", TEST_ALL_ZERO);
    } else {
        fp = fopen(TEST_ALL_ZERO, "w");
        for (int i = 0; i < 8192; i++) {
            putc('\0', fp);
        }
        fclose(fp);
    }

    if (access(TEST_ALL_GREEN, F_OK) == 0) {
        printf("NOT overwriting %s\n", TEST_ALL_GREEN);
    } else {
        fp = fopen(TEST_ALL_GREEN, "w");
        for (int i = 0; i < 4096; i++) {
            putc(0x2a, fp);
            putc(0x55, fp);
        }
        fclose(fp);
    }

    if (access(TEST_NO_MATCH, F_OK) == 0) {
        printf("NOT overwriting %s\n", TEST_NO_MATCH);
    } else {
        fp = fopen(TEST_NO_MATCH, "w");
        WriteNoMatch(fp);
        fclose(fp);
    }

    if (access(TEST_HALF_HALF, F_OK) == 0) {
        printf("NOT overwriting %s\n", TEST_HALF_HALF);
    } else {
        fp = fopen(TEST_HALF_HALF, "w");
        for (int i = 0; i < 4096; i++) {
            putc(0x00, fp);
        }
        WriteNoMatch(fp);
        fclose(fp);
        truncate(TEST_HALF_HALF, 8192);
    }


    return 0;
}
