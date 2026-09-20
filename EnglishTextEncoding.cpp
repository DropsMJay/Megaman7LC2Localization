// TextEncoding.cpp
#include "pch.h"
#include "EnglishTextEncoding.h"
#include "EnglishCharTable.h"
#include "Logging.h"

std::string EncodeLineToGameBytes(const std::string& utf8Line)
{
    std::string out;
    out.reserve(utf8Line.size());

    size_t i = 0;
    while (i < utf8Line.size()) {
        unsigned char lead = static_cast<unsigned char>(utf8Line[i]);
        char32_t codepoint = 0;
        size_t extraBytes = 0;

        if ((lead & 0x80) == 0x00) {          // 0xxxxxxx
            codepoint = lead;
            extraBytes = 0;
        } else if ((lead & 0xE0) == 0xC0) {   // 110xxxxx
            codepoint = lead & 0x1F;
            extraBytes = 1;
        } else if ((lead & 0xF0) == 0xE0) {   // 1110xxxx
            codepoint = lead & 0x0F;
            extraBytes = 2;
        } else if ((lead & 0xF8) == 0xF0) {   // 11110xxx
            codepoint = lead & 0x07;
            extraBytes = 3;
        } else {
            i++; // invalid lead byte -- skip it
            continue;
        }

        if (i + extraBytes >= utf8Line.size()) {
            break; // truncated multi-byte sequence, stop here
        }

        bool valid = true;
        for (size_t b = 1; b <= extraBytes; b++) {
            unsigned char cont = static_cast<unsigned char>(utf8Line[i + b]);
            if ((cont & 0xC0) != 0x80) { valid = false; break; }
            codepoint = (codepoint << 6) | (cont & 0x3F);
        }
        i += extraBytes + 1;
        if (!valid) continue;

        auto it = g_charToByte.find(codepoint);
        if (it != g_charToByte.end()) {
            out.push_back(static_cast<char>(it->second));
        } else {
            LogLine("WARNING: no game byte mapped for U+%04X in line \"%s\" -- using '?'\n",
                static_cast<unsigned int>(codepoint), utf8Line.c_str());
            out.push_back('?');
        }
    }

    return out;
}