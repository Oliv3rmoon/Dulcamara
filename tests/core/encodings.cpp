/*
 * Dulcamara - liblcf's encodings without ICU (tools/patches/liblcf-iconv.patch): the guess at a game's
 * encoding from its bytes, and the conversion to UTF-8 and back through iconv.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <lcf/encoder.h>
#include <lcf/reader_util.h>

#include <cstdio>
#include <string>
#include <vector>

static int failures;

static void expect(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static std::string first(const std::string &bytes)
{
    const auto list = lcf::ReaderUtil::DetectEncodings(bytes);
    return list.empty() ? std::string() : list.front();
}

int main()
{
    // "こんにちは、世界。冒険の始まりです" in Shift-JIS
    const std::string sjis = "\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd\x81\x41\x90\xa2\x8a\x45\x81\x42"
                             "\x96\x60\x8c\xaf\x82\xcc\x8e\x6e\x82\xdc\x82\xe8\x82\xc5\x82\xb7";
    // "Привет, мир! Начать игру" in windows-1251
    const std::string cp1251 = "\xcf\xf0\xe8\xe2\xe5\xf2, \xec\xe8\xf0! \xcd\xe0\xf7\xe0\xf2\xfc \xe8\xe3\xf0\xf3";
    // "Café crème, Übung, señor" in windows-1252
    const std::string cp1252 = "Caf\xe9 cr\xe8me, \xdc" "bung, se\xf1or";
    // "안녕하세요 모험을 시작합니다" in CP949 (EUC-KR)
    const std::string cp949 = "\xbe\xc8\xb3\xe7\xc7\xcf\xbc\xbc\xbf\xe4 \xb8\xf0\xc7\xe8\xc0\xbb "
                               "\xbd\xc3\xc0\xdb\xc7\xd5\xb4\xcf\xb4\xd9";

    expect(first(sjis) == "ibm-943_P15A-2003", "Shift-JIS text is guessed as Japanese (CP932)");
    expect(first(cp1251) == "ibm-5347_P100-1998", "windows-1251 text is guessed as Cyrillic");
    expect(first(cp1252) == "ibm-5348_P100-1997", "windows-1252 text is guessed as Western");
    expect(first(cp949) == "windows-949-2000", "CP949 text is guessed as Korean");
    expect(first("New Game") == "ibm-5348_P100-1997", "plain ASCII is Western");
    expect(first("Caf\xc3\xa9 cr\xc3\xa8me") == "UTF-8", "UTF-8 text is UTF-8");

    const auto all = lcf::ReaderUtil::DetectEncodings(sjis);
    bool has_western = false;
    for (const auto &e : all)
        has_western |= e == "ibm-5348_P100-1997";
    expect(all.size() > 1 && has_western, "the guess offers more than one encoding, Western among them");

    expect(lcf::ReaderUtil::Recode(sjis, "ibm-943_P15A-2003") ==
               "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf\xe3\x80\x81\xe4\xb8\x96\xe7\x95\x8c"
               "\xe3\x80\x82\xe5\x86\x92\xe9\x99\xba\xe3\x81\xae\xe5\xa7\x8b\xe3\x81\xbe\xe3\x82\x8a\xe3\x81\xa7"
               "\xe3\x81\x99",
           "Shift-JIS is converted to UTF-8");
    expect(lcf::ReaderUtil::Recode(cp1251, "ibm-5347_P100-1998") ==
               "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\xbc\xd0\xb8\xd1\x80! \xd0\x9d\xd0\xb0\xd1\x87"
               "\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb8\xd0\xb3\xd1\x80\xd1\x83",
           "windows-1251 is converted to UTF-8");
    expect(lcf::ReaderUtil::Recode("\\", "ibm-943_P15A-2003") == "\\", "a backslash stays a backslash in CP932");
    expect(lcf::ReaderUtil::Recode(cp1252, "1252") == "Caf\xc3\xa9 cr\xc3\xa8me, \xc3\x9c" "bung, se\xc3\xb1or",
           "code page numbers work too");

    lcf::Encoder encoder("ibm-943_P15A-2003");
    expect(encoder.IsOk(), "an encoder opens for CP932");
    std::string text = sjis;
    encoder.Encode(text);
    encoder.Decode(text);
    expect(text == sjis, "Shift-JIS survives a round trip through UTF-8");
    lcf::Encoder broken("no-such-encoding");
    expect(!broken.IsOk(), "an unknown encoding is refused");

    std::printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
