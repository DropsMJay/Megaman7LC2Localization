// EnglishTextEncoding.h
// Converts translated text (UTF-8, as written in GameTextUS.json) into the
// single-byte-per-character format the game's font system expects.
// See EnglishTextEncoding.h for the actual character -> byte lookup table.
#pragma once
#include <string>

std::string EncodeLineToGameBytes(const std::string& utf8Line);