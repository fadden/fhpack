/*
 * fhpack, an Apple II hi-res picture compressor.
 * By Andy McFadden
 * Version 1.0.1, August 2023
 *
 * Copyright 2015 by faddenSoft.  All Rights Reserved.
 * See the LICENSE.txt file for distribution terms (Apache 2.0).
 *
 * Under Linux, you can build it with just:
 *   g++ -O2 fhpack.cpp -o fhpack
 */
// TODO: prompt before overwriting output file (add "-f" to force)

/*
Format summary:

LZ4FH (FH is "fadden's hi-res") is similar to LZ4 (http://lz4.org) in
that the output is byte-oriented and has two kinds of chunks: "string of
literals" and "match".  The format has been modified to make it easier
(and faster) to decode on a 6502.

As with LZ4, the goal is to get reasonable compression ratios with an
extremely fast decoder.  On a CPU with 8-bit index registers, there is
a distinct advantage to keeping copy lengths under 256 bytes.  Since the
goal is to compress hi-res graphics, runs of identical bytes tend to be
fairly short anyway -- the interleaved nature means that solid blocks of
color aren't necessarily contiguous in memory -- so the ability to encode
runs of arbitrary length adds baggage with little benefit.

Files should use file type $08 (FOT) with auxtype $8066 (vendor-specific,
0x66 is 'f').

The format is very similar to LZ4, with a few key differences.  It
retains the idea of encoding the lengths of the next literal string
and next match in a single byte (4 bits each), so it is most efficient
when matches and literals alternate.

 file:
  1 byte : 0x66 - format magic number for version 1
    Not strictly necessary, but gives a hint if the images end up on
    a DOS 3.3 disk where there's no dedicated file type.
  [ ...one or more chunks follow... ]

 chunk:
  1 byte : length of literal string (hi 4 bits) and match (lo 4 bits)
    A literal-string len of zero indicates no literals (match follows
    match).  A literal-string len of 15 indicates that the match is
    at least 15, and the next byte must be added to it.  The match
    len is stored as (length - 4), allowing us to represent a match
    of length 4 to 18 with 4 bits.  A match len of 15 indicates that an
    additional byte is needed.
  1 byte : (optional) continuation of literal len, 0 - 240
    Add 15 to get a match length of 15 - 255.
  N bytes: 0 - 255 literal values

  1 byte : (optional) continuation of match len, 0 - 236 -or- 253/254
    Add 15 to get 15-251.  Factoring in the minimum match length of 4
    yields 19 - 255.  A value of 253 indicates no match (literals
    follow literals).  This is generally very rare, and is actually
    impossible if we overwrite the screen holes as that will guarantee
    a match every 120 literals.  A value of 254 indicates end-of-data.
  2 bytes: (if match) offset to match
    The offset is from the start of the output buffer, *not* back
    from the current position.  That way, if we're writing the output
    to $2000, instead of doing a 16-bit subtraction we can just
    ORA #$20 into the high byte.

We could save a byte by limiting the match distance to 8 bits (and probably
making it relative to the current position), but the interleaved layout of
the hi-res screen tends to spread things apart.  It won't really improve
our speed, which is what we're mostly concerned with.

The use of an explicit end indicator means we don't have to constantly
check to see if we've consumed enough input or produced enough output.
Unlike LZ4, we need to support adjacent runs of literals, so we already
need a special-case check on the match length.  It also means we can
choose to trim the file to $1ff8, losing the final "hole", or retain the
original file length.

Note that, in LZ4, the match offset comes before any optional match
length extensions, while in LZ4FH it comes after.  This allows the match
offset to be omitted when there's no match.  (This was not useful in LZ4
because literals-follow-literals doesn't occur.)

Expansion of uncompressible data is possible, but minimal.  The worst
case is a file with no matches.  We add three bytes of overhead for
every 255 literals (4/4 byte, 1 for literal len extension, 1 for match
len extension that holds the "no match" symbol).  Globally we add +1 for
the magic number.  The "end-of-data" symbol replaces the "no match"
symbol, so overall it's int(ceil(8192/255)) * 33 + 1 = 100 bytes.
*/
/*
Implementation notes:

The compression code uses an exhaustive brute-force search for matches.
The "greedy" approach is very slow, the "optimal" approach is extremely
slow.  It executes quickly on a modern machine, but would take a long
time to run on an Apple II.  On the bright side, with "greedy" parsing
it uses very little memory, and an optimized 6502/65816 implementation
might run in a reasonable amount of time.

Unrelated to the compression is the handling of the "screen holes".
Of the hi-res screens 8192 bytes, 512 are invisible.  We can teach the
compression code to skip over them, but that will require additional
code and will interrupt our literal/match strings every 120 bytes, so
it's better to alter the contents of the holes so that they blend into
the surrounding data and handle them as a match string.

Sometimes filling holes with a nearby pattern is not a win.  This is
particularly noticeable for the old digitized images in the "contrib"
folder, which have widely varying pixel values near the edges.  It turns
out we do slightly better by zeroing the holes out, which allows them
to match previous holes.  Also, sometimes there are patterns in the
file that happen to match eight zeroes followed by a splash of color.

Generally speaking the difference in output size is a few dozen bytes,
though in rare cases it can noticeably improve (-200) or cost (+50).
We resolve this conundrum by compressing the file twice and using whichever
works best.

*/

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

enum ProgramMode {
    MODE_UNKNOWN, MODE_COMPRESS, MODE_UNCOMPRESS, MODE_TEST
};

#define MAX_SIZE            8192
#define MIN_SIZE            (MAX_SIZE - 8)  // without final screen hole
#define MAX_EXPANSION       100             // ((MAX_SIZE/255)+1) * 3 + 1

#define MIN_MATCH_LEN       4
#define MAX_MATCH_LEN       255
#define MAX_LITERAL_LEN     255
#define INITIAL_LEN         15

#define EMPTY_MATCH_TOKEN   253
#define EOD_MATCH_TOKEN     254

#define LZ4FH_MAGIC         0x66

//#define DEBUG_MSGS
#ifdef DEBUG_MSGS
# define DBUG(x) printf x
#else
# define DBUG(x)
#endif

/*
 * Print usage info.
 */
static void usage(const char* argv0)
{
    fprintf(stderr, "fhpack v1.0.1 by Andy McFadden -*- ");
    fprintf(stderr, "Copyright 2015 by faddenSoft\n");
    fprintf(stderr,
        "Source code available from https://github.com/fadden/fhpack\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  fhpack {-c|-d} [-h] [-1|-9] infile outfile\n\n");
    fprintf(stderr, "  fhpack {-t} [-h] [-1|-9] infile1 [infile2...] \n\n");
    fprintf(stderr, "Use -c to compress, -d to decompress, -t to test\n");
    fprintf(stderr, " -h: don't fill or remove hi-res screen holes\n");
    fprintf(stderr, " -9: high compression (default)\n");
    fprintf(stderr, " -1: fast compression\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example: fhpack -c foo.pic foo.lz4fh\n");
}


/*
 * Zero out the "screen holes".
 */
void zeroHoles(uint8_t* inBuf)
{
    uint8_t* inPtr = inBuf + 120;

    while (inPtr < inBuf + MAX_SIZE) {
        memset(inPtr, 0, 8);
        inPtr += 128;
    }
}

/*
 * Fill in the "screen holes" in the image.  The hi-res page has
 * three 40-byte chunks of visible data, followed by 8 bytes of unseen
 * data (padding it to 128).
 *
 * Instead of simply zeroing them out, we want to examine the data that
 * comes before and after, copying whichever seems best into the hole.
 * If there's a repeating color pattern (2a 55 2a 55), the hole just
 * becomes part of the string, and will be handled as part of a long match.
 *
 * We can match the bytes that appear before or after the hole.
 * Ideally we'd use whichever yields the longest run.
 *
 * "inBuf" holds MAX_SIZE bytes.
 */
void fillHoles(uint8_t* inBuf)
{
    uint8_t* inPtr = inBuf + 120;
    while (inPtr < inBuf + MAX_SIZE) {
        // check to see if the bytes that follow are a better match
        // ("greedy" parsing can be suboptimal)
        uint8_t* checkp = inPtr + 8;
        bool useAfter = false;
        if (checkp < inBuf + MAX_SIZE) {
            if (checkp[0] == checkp[2] && checkp[1] == checkp[3]) {
                DBUG(("  bytes-after looks good at +0x%04lx\n",
                        checkp - inBuf));
                useAfter = true;
            } else {
                DBUG(("  bytes-before used at +0x%04lx\n", checkp - inBuf));
            }
        } else {
            DBUG(("  bytes-before used at end +0x%04lx\n", checkp - inBuf));
        }

        // Do an 8-byte overlapping copy.  We can overlap by 2 bytes
        // or 4 bytes depending on whether we want a 16-bit or 32-bit
        // repeating pattern.
        if (useAfter) {
            for (int i = 7; i >= 0; i--) {
                inPtr[i] = inPtr[i + 2];
            }
        } else {
            for (int i = 0; i < 8; i++) {
                inPtr[i] = inPtr[i - 2];
            }
        }

        inPtr += 128;
    }
}

/*
 * Computes the number of characters that match.  Stops when it finds
 * a mismatching byte, or "count" is reached.
 */
size_t getMatchLen(const uint8_t* str1, const uint8_t* str2, size_t count)
{
    size_t matchLen = 0;
    while (count-- && *str1++ == *str2++) {
        matchLen++;
    }
    return matchLen;
}

/*
 * Finds a match for the string at "matchPtr", in the buffer pointed
 * to by "inBuf" with length "inLen".  "matchPtr" must be inside "inBuf".
 *
 * We explicitly allow data to copy over itself, so a run of 200 0x00
 * bytes could be represented by a literal 0x00 followed immediately
 * by a match of length 199.  We do need to ensure that the initial
 * literal(s) go out first, though, so we use "maxStartOffset" to
 * restrict where matches may be found.
 *
 * Returns the length of the longest match found, with the match
 * offset in "*pMatchOffset".
 */
size_t findLongestMatch(const uint8_t* matchPtr, const uint8_t* inBuf,
    size_t inLen, size_t* pMatchOffset)
{
    size_t maxStartOffset = matchPtr - inBuf;
    size_t longest = 0;
    size_t longestOffset = 0;
    DBUG(("  findLongestMatch: maxSt=%zd\n", maxStartOffset));

    // Brute-force scan through the buffer.  Start from the beginning,
    // and continue up to the point we've generated until now.  (We
    // can't search the *entire* buffer right away because the decoder
    // can only copy matches from previously-decoded data.)
    for (size_t ii = 0; ii < maxStartOffset; ii++) {
        // Limit the length of the match by the length of the buffer.
        // We don't want the match code to go wandering off the end.
        // The match source is always earlier than matchPtr, so we
        // want to cap the length based on the distance from matchPtr
        // to the end of the buffer.
        size_t maxMatchLen = inLen - (matchPtr - inBuf);
        if (maxMatchLen > MAX_MATCH_LEN) {
            maxMatchLen = MAX_MATCH_LEN;
        }
        if (maxMatchLen < MIN_MATCH_LEN) {
            // too close to end of buffer, no point continuing
            break;
        }

        //DBUG(("  maxMatchLen is %zd\n", maxMatchLen));

        size_t matchLen = getMatchLen(matchPtr, inBuf + ii, maxMatchLen);
        if (matchLen > longest) {
            longest = matchLen;
            longestOffset = ii;
        }
        if (matchLen == maxMatchLen) {
            // Not going to find a longer one -- any future matches
            // will be the same length or shorter.
            break;
        }
    }


    *pMatchOffset = longestOffset;
    return longest;
}

/*
 * Compress a buffer, from "inBuf" to "outBuf".
 *
 * The input buffer holds between MIN_SIZE and MAX_SIZE bytes (inclusive),
 * depending on the length of the source material and whether or not
 * we're attempting to preserve the screen holes.
 *
 * Returns the amount of data in "outBuf" on success, or 0 on failure.
 */
size_t compressBufferOptimally(uint8_t* outBuf, const uint8_t* inBuf,
    size_t inLen)
{
    // Optimal parsing for data compression is a lot like computing the
    // shortest distance between two points in a directed graph.  For
    // each location, there are two possible "paths": a literal at this
    // point, which advances us one byte forward, or a match at this
    // point, which takes us several bytes forward.
    //
    // We walk through the file backward.  At each position, we compute
    // whether or not a match exists, and then determine the length from
    // the current position to the end depending on whether we handle
    // the value as a literal or the start of a match.  When we reach the
    // start of the file, we generate output by walking forward, selecting
    // the path based on whether a literal or match results in the best
    // outcome.
    struct OptNode {
        size_t totalCost;           // running total "best" length
        size_t matchLength;         // zero if no match or literal is best
        size_t matchOffset;

        size_t literalLength;       // running total of literal run length
    };
    OptNode* optList = (OptNode*) calloc(1, (inLen+1) * sizeof(OptNode));

    //
    // Pass 1: determine optimal path
    //

    for (unsigned int i = inLen - 1; i < inLen; i--) {
        size_t costForMatch, costForLiteral;

        // First consider the "match" path.  It doesn't matter what
        // follows the match, as that has no local effect on the output
        // length.
        size_t matchOffset;
        size_t longestMatch = findLongestMatch(inBuf + i, inBuf, inLen,
                &matchOffset);
        if (longestMatch < MIN_MATCH_LEN) {
            // no match to consider; leave optList[] values at zero
            costForMatch = MAX_SIZE * 2;   // arbitrary large value
        } else {
            // 4-14 bytes, fits in mixed-len byte
            optList[i].matchLength = longestMatch;
            optList[i].matchOffset = matchOffset;

            // total is previous total + 3 for match
            costForMatch = optList[i + longestMatch].totalCost + 3;
            if (longestMatch >= INITIAL_LEN) {
                costForMatch++;
            }
        }

        // Now consider the "literal" path.  If the next node is a
        // literal, we add on to the existing run.  If it's a match,
        // we're a length-1 literal.
        if (i == inLen - 1) {
            // special-case start (essentially a 1-byte file)
            optList[i].literalLength = 1;
            optList[i].totalCost = 2;
            costForLiteral = 2;        // mixed-len byte + literal
        } else {
            if (optList[i+1].matchLength != 0) {
                // next is match
                optList[i].literalLength = 1;
                costForLiteral = 1;    // literal; mixed-len byte in match
            } else if (optList[i+1].literalLength == MAX_LITERAL_LEN) {
                // next is max-length literal, start a new one
                optList[i].literalLength = 1;
                costForLiteral = 3;    // mixed-len byte + literal + nomatch
            } else {
                // next is sub-max-length literal, join it
                size_t newLiteralLen = optList[i+1].literalLength + 1;
                optList[i].literalLength = newLiteralLen;
                costForLiteral = 1;

                if (newLiteralLen == INITIAL_LEN) {
                    // just hit 15, now need the extension byte
                    costForLiteral++;
                }
            }
            costForLiteral += optList[i + 1].totalCost;
        }

        if (costForLiteral > costForMatch) {
            // use the match
            assert(longestMatch != 0);
            optList[i].totalCost = costForMatch;
            DBUG(("0x%04x use-mat [l=%zd m=%zd] (len=%zd off=0x%04zx) --> 0x%04zx\n",
                    i, costForLiteral, costForMatch, longestMatch,
                    matchOffset, optList[i].totalCost));
        } else {
            // use the literal -- zero the matchLength as a flag
            optList[i].matchLength = 0;
            optList[i].totalCost = costForLiteral;
            DBUG(("0x%04x use-lit [l=%zd m=%zd] (len=%zd) --> 0x%04zx\n",
                    i, costForLiteral, costForMatch, optList[i].literalLength,
                    optList[i].totalCost));
        }
    }

    // add one for the magic number; does not include end-of-data marker
    // (which will be +1 if the last thing is a literal, +2 if a match)
    size_t predictedLength = optList[0].totalCost + 1;
    DBUG(("predicted length is %zd\n", predictedLength));


    //
    // Pass 2: generate output from optimal path
    //

    //const uint8_t* inPtr = inBuf;
    uint8_t* outPtr = outBuf;

    *outPtr++ = LZ4FH_MAGIC;

    const uint8_t* literalSrcPtr = NULL;
    size_t numLiterals = 0;

    for (unsigned int i = 0; i < inLen; ) {
        if (optList[i].matchLength == 0) {
            // no match at this point, select literals
            if (numLiterals != 0) {
                // Previous entry was literals.  Because we parsed it
                // backwards, we can end up with 32 literals followed
                // by 255 literals, rather than the other way around.
                DBUG(("  output literal-literal (%zd)\n", numLiterals));
                if (numLiterals < INITIAL_LEN) {
                    // output 0-14 literals
                    *outPtr++ = (numLiterals << 4) | 0x0f;
                } else {
                    // output 15+(0-240) literals
                    *outPtr++ = 0xff;
                    *outPtr++ = numLiterals - INITIAL_LEN;
                }
                memcpy(outPtr, literalSrcPtr, numLiterals);
                outPtr += numLiterals;
                *outPtr++ = EMPTY_MATCH_TOKEN;
            }
            numLiterals = optList[i].literalLength;
            literalSrcPtr = inBuf + i;

            // advance to next node
            i += numLiterals;
        } else {
            // found a match, output previous literals first
            size_t longestMatch = optList[i].matchLength;
            size_t matchOffset = optList[i].matchOffset;
            size_t adjustedMatch = longestMatch - MIN_MATCH_LEN;

            // Start by emitting the 4/4 length byte.
            uint8_t mixedLengths;
            if (adjustedMatch <= INITIAL_LEN) {
                mixedLengths = adjustedMatch;
            } else {
                mixedLengths = INITIAL_LEN;
            }
            if (numLiterals <= INITIAL_LEN) {
                mixedLengths |= numLiterals << 4;
            } else {
                mixedLengths |= INITIAL_LEN << 4;
            }
            DBUG(("  match len=%zd off=0x%04zx lits=%zd mix=0x%02x\n",
                longestMatch, matchOffset, numLiterals,
                mixedLengths));
            *outPtr++ = mixedLengths;

            // Output the literals, starting with the extended length.
            if (numLiterals >= INITIAL_LEN) {
                *outPtr++ = numLiterals - INITIAL_LEN;
            }
            memcpy(outPtr, literalSrcPtr, numLiterals);
            outPtr += numLiterals;
            numLiterals = 0;
            literalSrcPtr = NULL;       // debug/sanity check

            // Now output the match, starting with the extended length.
            if (adjustedMatch >= INITIAL_LEN) {
                *outPtr++ = adjustedMatch - INITIAL_LEN;
            }
            *outPtr++ = matchOffset & 0xff;
            *outPtr++ = (matchOffset >> 8) & 0xff;

            i += longestMatch;
        }
    }

    // housekeeping check -- factor in end-of-data circumstances
    predictedLength++;
    if (numLiterals == 0) {
        predictedLength++;
    }

    // Dump any remaining literals, with the end-of-data indicator
    // in the match len.
    DBUG(("ending with numLiterals=%zd\n", numLiterals));
    if (numLiterals < INITIAL_LEN) {
        // 0-14 literals, only need the nibble
        *outPtr++ = (numLiterals << 4) | 0x0f;
    } else {
        // 15-255 literals, need the extra byte
        *outPtr++ = 0xff;
        *outPtr++ = numLiterals - INITIAL_LEN;
    }
    memcpy(outPtr, literalSrcPtr, numLiterals);
    outPtr += numLiterals;

    *outPtr++ = EOD_MATCH_TOKEN;

    DBUG(("Predicted length %zd, actual %ld\n",
        predictedLength, outPtr - outBuf));

    free(optList);
    return outPtr - outBuf;
}

/*
 * Compress a buffer, from "inBuf" to "outBuf".
 *
 * The input buffer holds between MIN_SIZE and MAX_SIZE bytes (inclusive),
 * depending on the length of the source material and whether or not
 * we're attempting to preserve the screen holes.
 *
 * Returns the amount of data in "outBuf" on success, or 0 on failure.
 */
size_t compressBufferGreedily(uint8_t* outBuf, const uint8_t* inBuf,
    size_t inLen)
{
    const uint8_t* inPtr = inBuf;
    uint8_t* outPtr = outBuf;

    const uint8_t* literalSrcPtr = NULL;
    size_t numLiterals = 0;

    *outPtr++ = LZ4FH_MAGIC;

    // Basic strategy: walk forward, searching for a match.  When we
    // find one, output the literals then the match.
    //
    // If the literal would cause us to exceed the maximum literal
    // length, output the previous literals with a "no match" indicator.
    while (inPtr < inBuf + inLen) {
        DBUG(("Loop: off 0x%08lx\n", inPtr - inBuf));

        // sanity-check on MAX_EXPANSION value
        assert(outPtr - outBuf < MAX_SIZE + MAX_EXPANSION);

        size_t matchOffset;
        size_t longestMatch = findLongestMatch(inPtr, inBuf, inLen,
                &matchOffset);
        if (longestMatch < MIN_MATCH_LEN) {
            // No good match found here, emit as literal.
            if (numLiterals == MAX_LITERAL_LEN) {
                // We've maxed out the literal string length.  Emit
                // the previously literals with an empty match indicator.
                DBUG(("  max literals reached\n"));
                *outPtr++ = 0xff;       // literal-len=15, match-len=15
                *outPtr++ = MAX_LITERAL_LEN - INITIAL_LEN;  // 240
                memcpy(outPtr, literalSrcPtr, numLiterals);
                outPtr += numLiterals;

                // Emit empty match indicator.
                *outPtr++ = EMPTY_MATCH_TOKEN;

                // Reset literal len, continue.
                numLiterals = 0;
            }
            if (numLiterals == 0) {
                // Start of run of literals.  Save pointer to data.
                literalSrcPtr = inPtr;
            }
            numLiterals++;
            inPtr++;
        } else {
            // Good match found.
            size_t adjustedMatch = longestMatch - MIN_MATCH_LEN;

            // Start by emitting the 4/4 length byte.
            uint8_t mixedLengths;
            if (adjustedMatch <= INITIAL_LEN) {
                mixedLengths = adjustedMatch;
            } else {
                mixedLengths = INITIAL_LEN;
            }
            if (numLiterals <= INITIAL_LEN) {
                mixedLengths |= numLiterals << 4;
            } else {
                mixedLengths |= INITIAL_LEN << 4;
            }
            DBUG(("  match len=%zd off=0x%04zx lits=%zd mix=0x%02x\n",
                longestMatch, matchOffset, numLiterals,
                mixedLengths));
            *outPtr++ = mixedLengths;

            // Output the literals, starting with the extended length.
            if (numLiterals >= INITIAL_LEN) {
                *outPtr++ = numLiterals - INITIAL_LEN;
            }
            memcpy(outPtr, literalSrcPtr, numLiterals);
            outPtr += numLiterals;
            numLiterals = 0;
            literalSrcPtr = NULL;       // debug/sanity check

            // Now output the match, starting with the extended length.
            if (adjustedMatch >= INITIAL_LEN) {
                *outPtr++ = adjustedMatch - INITIAL_LEN;
            }
            *outPtr++ = matchOffset & 0xff;
            *outPtr++ = (matchOffset >> 8) & 0xff;
            inPtr += longestMatch;
        }
    }

    // Dump any remaining literals, with the end-of-data indicator
    // in the match len.
    DBUG(("ending with numLiterals=%zd\n", numLiterals));
    if (numLiterals < INITIAL_LEN) {
        // 0-14 literals, only need the nibble
        *outPtr++ = (numLiterals << 4) | 0x0f;
    } else {
        // 15-255 literals, need the extra byte
        *outPtr++ = 0xff;
        *outPtr++ = numLiterals - INITIAL_LEN;
    }
    memcpy(outPtr, literalSrcPtr, numLiterals);
    outPtr += numLiterals;

    *outPtr++ = EOD_MATCH_TOKEN;

    return outPtr - outBuf;
}

/*
 * Uncompress from "inBuf" to "outBuf".
 *
 * Given valid data, "inLen" is not necessary.  It can be used as an
 * error check.
 *
 * Returns the uncompressed length on success, 0 on failure.
 */
size_t uncompressBuffer(uint8_t* outBuf, const uint8_t* inBuf, size_t inLen)
{
    uint8_t* outPtr = outBuf;
    const uint8_t* inPtr = inBuf;

    if (*inPtr++ != LZ4FH_MAGIC) {
        fprintf(stderr, "Missing LZ4FH magic\n");
        return 0;
    }

    while (true) {
        uint8_t mixedLen = *inPtr++;

        int literalLen = mixedLen >> 4;
        if (literalLen != 0) {
            if (literalLen == INITIAL_LEN) {
                literalLen += *inPtr++;
            }
            DBUG(("Literals: %d\n", literalLen));
            if ((outPtr - outBuf) + literalLen > (long) MAX_SIZE ||
                    (inPtr - inBuf) + literalLen > (long) inLen) {
                fprintf(stderr,
                    "Buffer overrun L: outPosn=%zd inPosn=%zd len=%d inLen=%ld\n",
                    outPtr - outBuf, inPtr - inBuf, literalLen, inLen);
                return 0;
            }
            memcpy(outPtr, inPtr, literalLen);
            outPtr += literalLen;
            inPtr += literalLen;
        } else {
            DBUG(("Literals: none\n"));
        }

        int matchLen = mixedLen & 0x0f;
        if (matchLen == INITIAL_LEN) {
            uint8_t addon = *inPtr++;
            if (addon == EMPTY_MATCH_TOKEN) {
                DBUG(("Match: none\n"));
                matchLen = - MIN_MATCH_LEN;
            } else if (addon == EOD_MATCH_TOKEN) {
                DBUG(("Hit end-of-data at 0x%04lx\n", outPtr - outBuf));
                break;      // out of while
            } else {
                matchLen += addon;
            }
        }

        matchLen += MIN_MATCH_LEN;
        if (matchLen != 0) {
            int matchOffset = *inPtr++;
            matchOffset |= (*inPtr++) << 8;
            DBUG(("Match: %d at %d\n", matchLen, matchOffset));
            // Can't use memcpy() here, because we need to guarantee
            // that the match is overlapping.
            uint8_t* srcPtr = outBuf + matchOffset;
            if ((outPtr - outBuf) + matchLen > MAX_SIZE ||
                    (srcPtr - outBuf) + matchLen > MAX_SIZE) {
                fprintf(stderr,
                    "Buffer overrun M: outPosn=%zd srcPosn=%zd len=%d\n",
                    outPtr - outBuf, srcPtr - outBuf, matchLen);
                return 0;
            }
            while (matchLen-- != 0) {
                *outPtr++ = *srcPtr++;
            }
        }
    }

    if (inPtr - inBuf != (long) inLen) {
        fprintf(stderr, "Warning: uncompress used only %ld of %zd bytes\n",
                inPtr - inBuf, inLen);
    }

    return outPtr - outBuf;
}

/*
 * Compress a file, from "inFileName" to "outFileName".
 *
 * Returns 0 on success.
 */
int compressFile(const char* outFileName, const char* inFileName,
    bool doPreserveHoles, bool useGreedyParsing)
{
    int result = -1;
    uint8_t inBuf1[MAX_SIZE];
    uint8_t inBuf2[MAX_SIZE];
    uint8_t verifyBuf[MAX_SIZE];
    uint8_t outBuf1[MAX_SIZE + MAX_EXPANSION];
    uint8_t outBuf2[MAX_SIZE + MAX_EXPANSION];
    uint8_t* outBuf = NULL;
    uint8_t* inBuf = NULL;
    size_t outSize, sourceLen, uncompressedLen;
    FILE* outfp = NULL;
    FILE* infp;

    infp = fopen(inFileName, "rb");
    if (infp == NULL) {
        perror("Unable to open input file");
        return -1;
    }

    if (outFileName != NULL) {
        outfp = fopen(outFileName, "wb");
        if (outfp == NULL) {
            perror("Unable to open output file");
            fclose(infp);
            return -1;
        }
    }

    fseek(infp, 0, SEEK_END);
    long fileLen = ftell(infp);
    rewind(infp);
    if (fileLen < MIN_SIZE || fileLen > MAX_SIZE) {
        fprintf(stderr, "ERROR: input file is %ld bytes, must be %d - %d\n",
            fileLen, MIN_SIZE, MAX_SIZE);
        goto bail;
    }

    // Read data into buffer.
    if (fread(inBuf1, 1, fileLen, infp) != (size_t) fileLen) {
        perror("Failed while reading data");
        goto bail;
    }

    if (doPreserveHoles) {
        // Don't modify the input.
        sourceLen = fileLen;        // retain original file length
        if (useGreedyParsing) {
            outSize = compressBufferGreedily(outBuf1, inBuf1, sourceLen);
        } else {
            outSize = compressBufferOptimally(outBuf1, inBuf1, sourceLen);
        }
        inBuf = inBuf1;
        outBuf = outBuf1;
    } else {
        sourceLen = MIN_SIZE;       // always drop the last 8 bytes
        memcpy(inBuf2, inBuf1, sourceLen);

        // try it twice, with zero-filled holes and content-filled holes

        size_t outSize1;
        zeroHoles(inBuf1);
        if (useGreedyParsing) {
            outSize1 = compressBufferGreedily(outBuf1, inBuf1, sourceLen);
        } else {
            outSize1 = compressBufferOptimally(outBuf1, inBuf1, sourceLen);
        }

        size_t outSize2;
        fillHoles(inBuf2);
        if (useGreedyParsing) {
            outSize2 = compressBufferGreedily(outBuf2, inBuf2, sourceLen);
        } else {
            outSize2 = compressBufferOptimally(outBuf2, inBuf2, sourceLen);
        }

        if (false) {     // save hole-punched output for examination
            FILE* foo = fopen("HOLES", "wb");
            fwrite(inBuf2, 1, MIN_SIZE, foo);
            fclose(foo);
        }

        if (outSize1 <= outSize2) {
            printf("  using zeroed-out holes (%zd vs. %zd)\n",
                outSize1, outSize2);
            outSize = outSize1;
            inBuf = inBuf1;
            outBuf = outBuf1;
        } else {
            printf("  using filled-in holes (%zd vs. %zd)\n",
                outSize2, outSize1);
            outSize = outSize2;
            inBuf = inBuf2;
            outBuf = outBuf2;
        }
    }

    if (outSize == 0) {
        fprintf(stderr, "Compression failed\n");
        goto bail;
    }
    DBUG(("*** outSize is %zd\n", outSize));

    // uncompress the data we just compressed
    memset(verifyBuf, 0xcc, sizeof(verifyBuf));
    uncompressedLen = uncompressBuffer(verifyBuf, outBuf, outSize);
    if (uncompressedLen != sourceLen) {
        fprintf(stderr, "ERROR: verify expanded %zd of expected %zd bytes\n",
            uncompressedLen, sourceLen);
        goto bail;
    }

    // byte-for-byte comparison
    for (size_t ii = 0; ii < sourceLen; ii++) {
        if (inBuf[ii] != verifyBuf[ii]) {
            fprintf(stderr,
                "ERROR: expansion mismatch (byte %zd, 0x%02x 0x%02x)\n",
                ii, inBuf[ii], verifyBuf[ii]);
            goto bail;
        }
    }
    DBUG(("Verification succeeded\n"));

    if (outfp != NULL) {
        /* write the data */
        if (fwrite(outBuf, 1, outSize, outfp) != outSize) {
            perror("Failed while writing data");
            goto bail;
        }
    } else {
        // must be in test mode
        printf("  success -- compressed len is %zd\n", outSize);
    }

    result = 0;

bail:
    fclose(infp);
    if (outfp != NULL) {
        fclose(outfp);
    }
    if (result != 0 && outFileName != NULL) {
        unlink(outFileName);
    }
    return result;
}

/*
 * Uncompress data from one file to another.
 *
 * Returns 0 on success.
 */
int uncompressFile(const char* outFileName, const char* inFileName)
{
    int result = -1;
    uint8_t inBuf[MAX_SIZE + MAX_EXPANSION];
    uint8_t outBuf[MAX_SIZE];
    size_t outSize;
    FILE* outfp = NULL;
    FILE* infp;

    infp = fopen(inFileName, "rb");
    if (infp == NULL) {
        perror("Unable to open input file");
        return -1;
    }

    outfp = fopen(outFileName, "wb");
    if (outfp == NULL) {
        perror("Unable to open output file");
        fclose(infp);
        return -1;
    }

    fseek(infp, 0, SEEK_END);
    long fileLen = ftell(infp);
    rewind(infp);
    if (fileLen < 10 || fileLen > MAX_SIZE + MAX_EXPANSION) {
        // 10 just ensures we have enough for magic number, chunk, eod
        fprintf(stderr, "ERROR: input file is %ld bytes, must be < %d\n",
            fileLen, MAX_SIZE + MAX_EXPANSION);
        goto bail;
    }

    // Read data into buffer.
    if (fread(inBuf, 1, fileLen, infp) != (size_t) fileLen) {
        perror("Failed while reading data");
        goto bail;
    }

    outSize = uncompressBuffer(outBuf, inBuf, fileLen);
    if (outSize == 0) {
        goto bail;
    }
    DBUG(("*** outSize is %zd\n", outSize));

    /* write the data */
    if (fwrite(outBuf, 1, outSize, outfp) != outSize) {
        perror("Failed while writing data");
        goto bail;
    }

    result = 0;

bail:
    fclose(infp);
    fclose(outfp);
    if (result != 0) {
        unlink(outFileName);
    }
    return result;
}

/*
 * Process args.
 */
int main(int argc, char* argv[])
{
    ProgramMode mode = MODE_UNKNOWN;
    bool doPreserveHoles = false;
    bool useGreedyParsing = false;
    bool wantUsage = false;
    int opt;

    while ((opt = getopt(argc, argv, "19cdth")) != -1) {
        switch (opt) {
        case '1':
            useGreedyParsing = true;
            break;
        case '9':
            useGreedyParsing = false;
            break;
        case 'c':
            if (mode == MODE_UNKNOWN) {
                mode = MODE_COMPRESS;
            } else {
                wantUsage = true;
            }
            break;
        case 'd':
            if (mode == MODE_UNKNOWN) {
                mode = MODE_UNCOMPRESS;
            } else {
                wantUsage = true;
            }
            break;
        case 't':
            if (mode == MODE_UNKNOWN) {
                mode = MODE_TEST;
            } else {
                wantUsage = true;
            }
            break;
        case 'h':
            doPreserveHoles = true;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (argc - optind < 1 ||
        (mode != MODE_TEST && argc - optind != 2))
    {
        wantUsage = true;
    }

    if (mode == MODE_UNKNOWN || wantUsage) {
        usage(argv[0]);
        return 2;
    }

    const char* inFileName = argv[optind];
    const char* outFileName = argv[optind+1];

    int result = 0;
    if (mode == MODE_COMPRESS) {
        printf("Compressing %s -> %s\n", inFileName, outFileName);
        result = compressFile(outFileName, inFileName, doPreserveHoles,
                useGreedyParsing);
    } else if (mode == MODE_UNCOMPRESS) {
        printf("Expanding %s -> %s\n", inFileName, outFileName);
        result = uncompressFile(outFileName, inFileName);
    } else {
        while (optind < argc) {
            printf("Testing %s\n", argv[optind]);
            result |= compressFile(NULL, argv[optind], doPreserveHoles,
                    useGreedyParsing);
            optind++;
        }
    }

    return (result != 0);
}

