fhpack - compression for Apple II hi-res images
===============================================

fhpack is a compression tool with a singular purpose: to compress
Apple II hi-res graphics images.  It uses a modified version of
the LZ4 compression format, called LZ4FH ("fadden's hi-res").


## Origins ##

I've had an idea for a project involving hi-res graphics compression
for several years, but didn't do much about it.  After learning
about [LZ4](http://lz4.org/), and seeing uncompressors written
for the [6502](http://pferrie.host22.com/misc/appleii.htm) and
[65816](http://www.brutaldeluxe.fr/products/crossdevtools/lz4/index.html),
I decided to see if I could apply LZ4 to hi-res images.

A few hi-res compressors were written back in The Day, usually employing
run-length encoding, which is easy to write and fast to encode and decode.
In the spirit of LZ4, I decided to put together an asymmetric codec,
meaning compression is very very slow, but uncompression is very very fast.

The result is a modified form of LZ4 that consistently beats LZ4-HC,
and generally comes close to (and occasionally beats) ShrinkIt's LZW/II.
The decoder is tiny and extremely fast, especially on the 65816 where
the bulk data copy instructions can be used.


## About the fhpack Tool ##

The compressor has two modes, similar to LZ4's "fast" and "high".  The
fast mode uses greedy parsing and is not particularly fast, while the
high-compression mode uses optimal parsing and takes 12 times as long.
Both employ simple brute-force algorithms, which we can get away with
because we're only compressing 8KB of data.  The high-compression mode
does about 4% better on average -- not huge, but not negligible.

Other compression programs, such as gzip, produce significantly smaller
output, but uncompression is much slower and requires more memory.

The comments in [fhpack.cpp](fhpack.cpp) describe the data format.

There is no implementation of the compression side for the 6502.
An implementation that uses greedy parsing is feasible, as the bulk of the
time is spent comparing 8-bit strings that are less than 256 bytes long,
and the 6502 series is pretty good at that.  The optimal parser could
theoretically be done on a machine with 128KB of RAM, but would take a
very long time to run.


#### Screen Holes ####

The hi-res screen has a curious interleaved structure that leaves "holes"
in memory -- parts of the frame buffer that don't affect what appears
on screen.  The screen layout is divided into 128-byte sections, with
120 bytes of visible data followed by an eight byte "hole".  The holes
tend to be filled with zeroes, though sometimes they may contain
garbage or program state.

fhpack can do one of three things with the screen holes:

 1. Preserve them.  This mode is enabled with the "-h" flag.  If you
    want the uncompressed data to exactly match the original, you
    must specify this flag.
 2. Fill them with zeroes.
 3. Fill them with a pattern that matches the data immediately before
    or after the hole.

In some cases #2 provides the best results, in others #3 wins.
The difference is usual minimal, with outliers in the 70-90 byte range.
On modern hardware fhpack runs very quickly, so when not in hole-preserve
mode the tool compresses everything twice, and keeps whichever approach
yielded the smallest output.


## Apple II Code and Demos ##

The 6502/65816 versions of the uncompressor (source and binaries), as
well as two slideshow applications written in Applesoft and a number
of sample files, are provided on the [attached disk images](fhpack_disks.zip)
(click "view raw" to download them from github).

There are six disk images.  The first three hold the slide show demo:

 * `LZ4FHDemo.do` (/LZ4FH, 140KB) - Source and object code for the
   uncompression routines, plus a few test images and the Applesoft
   "SLIDESHOW" program.
 * `UncompressedSlides.do` (/SLIDESHOW, 140KB) - A set of 16 uncompressed
   hi-res images.
 * `CompressedSlides.do` (/SLIDESHOW, 140KB) - A set of 42 compressed
   hi-res images.

To [view the demo](https://www.youtube.com/watch?v=sNBSd1oGGaU),
put the LZ4FHDemo image in slot 6 drive 1, and one
of the "slide" disks in slot 6 drive 2.  Boot the disk and "-SLIDESHOW".
Just hit return at the prompt to accept the default prefix.

The slideshow program will scan the specified directory and identify files
that appear to be compressed or uncompressed hi-res images.  It will
then start a slide show, moving through them as quickly as possible.
By swapping the compressed and uncompressed disks and restarting the
program, you can compare the performance with and without compression.
(For a 5.25" disk, it's generally faster to load a compressed image and
uncompress it than it is to load an uncompressed image.)


There is a second demo, called "HYPERSLIDE", which shows off the raw
performance by eliminating the disk accesses.  A set of 15 images is loaded
into just 24KB of memory -- overwriting BASIC.System -- and presented as a
slide show as quickly as possible.  The demo and selected images are on
this disk:

 * `HyperSlide.po` (/HYPERSLIDE, 140KB)

To [run the demo](https://www.youtube.com/watch?v=Wwg84nIkRZU), put
the disk image in slot 6 drive 1, boot the disk,
and "-HYPERSLIDE".  If you are running on a IIgs, you may want to try it
with the 65816 uncompressor, which is much faster than the 6502 version.
If you want to compute frame timings, you can set an iteration count,
and the slide show will beep at the start and end.

A larger set of images is available on a pair of 800KB disks.  One disk
has the compressed form, the other the uncompressed form:

 * `UncompressedImages.po` (/IMAGES, 800KB)
 * `CompressedImages.po` (/IMAGES.LZ4H, 800KB)

It's worth noting that the images on `CompressedSlides.do` take up about
135KB of disk space, but are about 104KB combined.  The rest of the space
is used up by filesystem overhead.  Storing them in a ShrinkIt archive
would be more efficient, but would also make them far more difficult
to unpack.


#### Decoder Performance ####

Running under AppleWin with "authentic" disk access speed enabled,
a slide show of uncompressed images runs at about 1.7 seconds per
image (about 0.6fps).  With compressed images the time varies, because
the size of the compressed image affects the amount of disk activity,
but it averages about 1.4 seconds per image (about 0.7 fps).

Removing disk activity from the equation, HyperSlide improves that to
about 3.7 fps, with very little variation between files.  The decode
time is dominated by byte copies, and we're always copying 8KB, so the
consistency is expected.

HyperSlide incurs a fair bit of overhead from Applesoft BASIC.  The
"blitz test", included on the LZ4FH demo disk, generates machine language
calls that uncompress the same image 100x, eliminating all overhead
(and simulating what HyperSlide could do if it weren't written in
BASIC).  The speed improves to 5.6 fps.

The most significant boost in speed comes from using the 65816 data
move instructions.  With a 65816 implementation, still running at 1MHz,
HyperSlide hits 6 fps, and BLITZTEST tops 12 fps.


#### Code Notes ####

The uncompressor takes as arguments the addresses of the compressed data
and the buffer to uncompress to.  These are poked into memory locations
$02FC and $02FE.  In the current implementation, the output buffer must
be $2000 or $4000 (the two hi-res pages).

Packed images use the FOT ($08) file type, with an auxtype of $8066
(0x66 is ASCII 'f').  These files can be viewed with
[CiderPress](http://a2ciderpress.com) v4.0.1 and later.


## Experimental Results ##

I grabbed a set of about 70 images, most from games, a few from early
"contributed program" disks.  The latter include what look like digitized
scans that don't compress especially well.

All images were compressed with LZ4 r131 in high-compression mode (`lz4
-9`), NuLib2 with LZW/II, and LZ4FH (`fhpack -9`).  fhpack output has a
one-byte magic number, while LZ4-HC has 15 bytes of headers and footers,
so for a fair "raw data" comparison the numbers should be adjusted
appropriately.

Most source images are 8192 bytes long, some are a few bytes shorter.

Image File              | LZ4-HC | fhpack | LZW/II |
----------------------- | -----: | -----: | -----: |
contrib/BABY.JANE       |  3664  |  3617  |  2851  |
contrib/CHARACTERS      |  1874  |  1836  |  1614  |
contrib/CHURCHILL       |  4759  |  4723  |  3749  |
contrib/DIP.CHIPS       |  3840  |  3785  |  3048  |
contrib/DOLLAR          |  3838  |  3790  |  3483  |
contrib/DOUBLE.BESSEL   |  3066  |  3010  |  2566  |
contrib/GIRLS.BEST.FRND |  5967  |  5933  |  4659  |
contrib/HOPALONG        |  3394  |  3331  |  2713  |
contrib/JOE.SENT.ME     |  7569  |  7546  |  7702  |
contrib/LADY.BE.GOOD    |  4119  |  4060  |  3292  |
contrib/MACROMETER      |  4405  |  4354  |  3613  |
contrib/MUSIC           |  1077  |  1030  |   929  |
contrib/RANDOM.LADY     |  5754  |  5720  |  5426  |
contrib/ROCKY.RACCOON   |  5864  |  5754  |  5598  |
contrib/SHAKESPEARE     |  4933  |  4884  |  4286  |
contrib/SPIRALLELLO     |  5722  |  5704  |  5345  |
contrib/SQUEEZE         |  3209  |  3163  |  2533  |
contrib/TEQUILA         |  5881  |  5828  |  5484  |
contrib/TEX             |  4463  |  4413  |  3677  |
contrib/TIME.MACHINE    |  3628  |  3578  |  3023  |
contrib/UNCLE.SAM       |  3057  |  3012  |  2784  |
contrib/WORLD.MAP       |  2792  |  2723  |  2429  |
games/ABM.TITLE         |  1387  |  1353  |  1581  |
games/ARCHON.TITLE      |  5607  |  5589  |  4991  |
games/AZTEC.TITLE       |  6378  |  6362  |  6414  |
games/BAM.TITLE         |  5380  |  5377  |  4902  |
games/BANDITS.TITLE     |  1442  |  1388  |  1376  |
games/BARDS.TALE.1      |  3853  |  3815  |  3523  |
games/BILESTOAD         |  2140  |  1559  |  2552  |
games/BORG.TITLE        |  2327  |  2559  |  2009  |
games/CAPT.GOODNIGHT    |  2839  |  2820  |  2726  |
games/CHOPLIFTER        |  1088  |  1007  |  1070  |
games/CRISIS.MT.GAME    |  4144  |  4085  |  4037  |
games/CRISIS.MT.TITLE   |  2290  |  2246  |  2384  |
games/DAVID.MIDNIGHT    |  3362  |  3322  |  3225  |
games/DEFENDER          |   728  |   696  |   678  |
games/EAMON.TITLE       |  3744  |  3665  |  3252  |
games/GALACTIC.EMPIRE   |  2161  |  2087  |  1956  |
games/GERMANY.1985      |  2566  |  2460  |  2390  |
games/HARD.HAT.MAC      |  1524  |  1457  |  1678  |
games/KADASH.DEMO       |  1796  |  1736  |  2120  |
games/KADASH.TITLE      |  5317  |  5294  |  5393  |
games/KARATE.TITLE      |  4040  |  3845  |  3952  |
games/KARATEKA.FORT     |  4050  |  3955  |  3452  |
games/KARATEKA.GAME     |   948  |   904  |  1108  |
games/LODE.RUNNER       |  1133  |  1102  |  1428  |
games/MARIO.BROS        |  1472  |  1406  |  1372  |
games/MAZE.CRAZE        |  2703  |  2659  |  2485  |
games/MICROWAVE.TITLE   |  2812  |  2737  |  2434  |
games/NIGHT.FLIGHT      |  1109  |  1024  |  1183  |
games/ODYSSEY.TITLE     |  3994  |  3953  |  3752  |
games/OUTWORLD          |  2222  |  2157  |  2296  |
games/PCS               |  1897  |  1837  |  1861  |
games/PCS.TITLE         |  1881  |  1841  |  1882  |
games/QUESTRON.DEMO     |  2569  |  2518  |  2253  |
games/QUESTRON.TITLE    |  1536  |  1499  |  1837  |
games/RASTER.BLASTER    |  2687  |  2636  |  2553  |
games/RESCUE.RAIDERS    |  5377  |  4961  |  4883  |
games/ROADWAR2K.TITLE   |  2063  |  1983  |  2068  |
games/SPARE.CHANGE      |  2058  |  2009  |  2268  |
games/STAR.MAZE         |  1253  |  1208  |  1600  |
games/STARSHIP.CMDR     |  1453  |  1427  |  1845  |
games/STELLAR.7         |  1629  |  1412  |  1845  |
games/SUNDOG.TITLE      |  3250  |  3188  |  3270  |
games/SWASHBUCK.GAME    |  4690  |  4608  |  4286  |
games/SWASHBUCK.TITLE   |  5077  |  5035  |  5085  |
games/TRANQUILITY       |  1409  |  1363  |  1273  |
games/ULT2.LORD.BRIT    |  1529  |  1514  |  1592  |
games/ULTIMA2.TITLE     |  2220  |  2176  |  2201  |
games/WASTELAND.TITLE   |  3540  |  3078  |  3510  |
games/WAYOUT            |  1691  |  1669  |  1864  |
games/WOLFEN.TITLE      |  2638  |  2588  |  2610  |
games/ZAXXON            |  1884  |  1862  |  1769  |
misc/CRAPS.TABLE        |  2286  |  2266  |  2548  |
misc/GHOSTBUST.LOGO     |  1829  |  1724  |  1594  |
misc/LINE.CHART         |  1753  |  1655  |  1578  |
misc/MICKEY             |  3369  |  3316  |  2945  |
misc/WHO.LOGO           |  1138  |  1084  |  1218  |
test/allgreen           |    63  |   137  |   215  |
test/allzero            |    62  |   136  |    38  |
test/nomatch            |  8211  |  7928  |  7414  |
TOTAL                   | 248473 | 242771 | 232201 |
                        |  37.4% |  36.5% |  34.9% |

Note: test/nomatch is not compressible by LZ4 encoding.  fhpack was able
to compress it because it zeroed out the "screen holes".  When processed
in hole-preservation mode, test/nomatch expands to 8292 bytes.

LZSS, which was used by HardPressed to get reasonable compression with
fast decode speeds, reduces the corpus to 243991 bytes (36.7%), making it
a viable alternative.  It's generally inferior to LZ4 as the maximum
match length and offset are much shorter, but that's not too significant
for hi-res images.  Literals are identified with individual flag bits,
rather than as runs of bytes, which reduces performance for long strings
of literals.
