
#include "keymaps.h"

static const name2keysym_t name2keysym[]={
/* ascii */
    { "space",                0x020},
    { "exclam",               0x021},
    { "quotedbl",             0x022},
    { "numbersign",           0x023},
    { "dollar",               0x024},
    { "percent",              0x025},
    { "ampersand",            0x026},
    { "apostrophe",           0x027},
    { "parenleft",            0x028},
    { "parenright",           0x029},
    { "asterisk",             0x02a},
    { "plus",                 0x02b},
    { "comma",                0x02c},
    { "minus",                0x02d},
    { "period",               0x02e},
    { "slash",                0x02f},
    { "0",                    0x030},
    { "1",                    0x031},
    { "2",                    0x032},
    { "3",                    0x033},
    { "4",                    0x034},
    { "5",                    0x035},
    { "6",                    0x036},
    { "7",                    0x037},
    { "8",                    0x038},
    { "9",                    0x039},
    { "colon",                0x03a},
    { "semicolon",            0x03b},
    { "less",                 0x03c},
    { "equal",                0x03d},
    { "greater",              0x03e},
    { "question",             0x03f},
    { "at",                   0x040},
    { "A",                    0x041},
    { "B",                    0x042},
    { "C",                    0x043},
    { "D",                    0x044},
    { "E",                    0x045},
    { "F",                    0x046},
    { "G",                    0x047},
    { "H",                    0x048},
    { "I",                    0x049},
    { "J",                    0x04a},
    { "K",                    0x04b},
    { "L",                    0x04c},
    { "M",                    0x04d},
    { "N",                    0x04e},
    { "O",                    0x04f},
    { "P",                    0x050},
    { "Q",                    0x051},
    { "R",                    0x052},
    { "S",                    0x053},
    { "T",                    0x054},
    { "U",                    0x055},
    { "V",                    0x056},
    { "W",                    0x057},
    { "X",                    0x058},
    { "Y",                    0x059},
    { "Z",                    0x05a},
    { "bracketleft",          0x05b},
    { "backslash",            0x05c},
    { "bracketright",         0x05d},
    { "asciicircum",          0x05e},
    { "underscore",           0x05f},
    { "grave",                0x060},
    { "a",                    0x061},
    { "b",                    0x062},
    { "c",                    0x063},
    { "d",                    0x064},
    { "e",                    0x065},
    { "f",                    0x066},
    { "g",                    0x067},
    { "h",                    0x068},
    { "i",                    0x069},
    { "j",                    0x06a},
    { "k",                    0x06b},
    { "l",                    0x06c},
    { "m",                    0x06d},
    { "n",                    0x06e},
    { "o",                    0x06f},
    { "p",                    0x070},
    { "q",                    0x071},
    { "r",                    0x072},
    { "s",                    0x073},
    { "t",                    0x074},
    { "u",                    0x075},
    { "v",                    0x076},
    { "w",                    0x077},
    { "x",                    0x078},
    { "y",                    0x079},
    { "z",                    0x07a},
    { "braceleft",            0x07b},
    { "bar",                  0x07c},
    { "braceright",           0x07d},
    { "asciitilde",           0x07e},

/* latin 1 extensions */
{ "nobreakspace",         0x0a0},
{ "exclamdown",           0x0a1},
{ "cent",         	  0x0a2},
{ "sterling",             0x0a3},
{ "currency",             0x0a4},
{ "yen",                  0x0a5},
{ "brokenbar",            0x0a6},
{ "section",              0x0a7},
{ "diaeresis",            0x0a8},
{ "copyright",            0x0a9},
{ "ordfeminine",          0x0aa},
{ "guillemotleft",        0x0ab},
{ "notsign",              0x0ac},
{ "hyphen",               0x0ad},
{ "registered",           0x0ae},
{ "macron",               0x0af},
{ "degree",               0x0b0},
{ "plusminus",            0x0b1},
{ "twosuperior",          0x0b2},
{ "threesuperior",        0x0b3},
{ "acute",                0x0b4},
{ "mu",                   0x0b5},
{ "paragraph",            0x0b6},
{ "periodcentered",       0x0b7},
{ "cedilla",              0x0b8},
{ "onesuperior",          0x0b9},
{ "masculine",            0x0ba},
{ "guillemotright",       0x0bb},
{ "onequarter",           0x0bc},
{ "onehalf",              0x0bd},
{ "threequarters",        0x0be},
{ "questiondown",         0x0bf},
{ "Agrave",               0x0c0},
{ "Aacute",               0x0c1},
{ "Acircumflex",          0x0c2},
{ "Atilde",               0x0c3},
{ "Adiaeresis",           0x0c4},
{ "Aring",                0x0c5},
{ "AE",                   0x0c6},
{ "Ccedilla",             0x0c7},
{ "Egrave",               0x0c8},
{ "Eacute",               0x0c9},
{ "Ecircumflex",          0x0ca},
{ "Ediaeresis",           0x0cb},
{ "Igrave",               0x0cc},
{ "Iacute",               0x0cd},
{ "Icircumflex",          0x0ce},
{ "Idiaeresis",           0x0cf},
{ "ETH",                  0x0d0},
{ "Eth",                  0x0d0},
{ "Ntilde",               0x0d1},
{ "Ograve",               0x0d2},
{ "Oacute",               0x0d3},
{ "Ocircumflex",          0x0d4},
{ "Otilde",               0x0d5},
{ "Odiaeresis",           0x0d6},
{ "multiply",             0x0d7},
{ "Ooblique",             0x0d8},
{ "Oslash",               0x0d8},
{ "Ugrave",               0x0d9},
{ "Uacute",               0x0da},
{ "Ucircumflex",          0x0db},
{ "Udiaeresis",           0x0dc},
{ "Yacute",               0x0dd},
{ "THORN",                0x0de},
{ "Thorn",                0x0de},
{ "ssharp",               0x0df},
{ "agrave",               0x0e0},
{ "aacute",               0x0e1},
{ "acircumflex",          0x0e2},
{ "atilde",               0x0e3},
{ "adiaeresis",           0x0e4},
{ "aring",                0x0e5},
{ "ae",                   0x0e6},
{ "ccedilla",             0x0e7},
{ "egrave",               0x0e8},
{ "eacute",               0x0e9},
{ "ecircumflex",          0x0ea},
{ "ediaeresis",           0x0eb},
{ "igrave",               0x0ec},
{ "iacute",               0x0ed},
{ "icircumflex",          0x0ee},
{ "idiaeresis",           0x0ef},
{ "eth",                  0x0f0},
{ "ntilde",               0x0f1},
{ "ograve",               0x0f2},
{ "oacute",               0x0f3},
{ "ocircumflex",          0x0f4},
{ "otilde",               0x0f5},
{ "odiaeresis",           0x0f6},
{ "division",             0x0f7},
{ "oslash",               0x0f8},
{ "ooblique",             0x0f8},
{ "ugrave",               0x0f9},
{ "uacute",               0x0fa},
{ "ucircumflex",          0x0fb},
{ "udiaeresis",           0x0fc},
{ "yacute",               0x0fd},
{ "thorn",                0x0fe},
{ "ydiaeresis",           0x0ff},

{NULL, 0},
};
