#!/usr/bin/env python3
"""檢查 app 的翻譯齊不齊。

原始碼裡給使用者看的每一句都寫成 L("繁體中文原文", 值…)，原文就是索引；其他語言的翻譯在
<語言>.lproj/Localizable.strings。這支程式把 Sources/*.swift 裡所有的 L("…") 找出來，和每一份翻譯對照：

  * 原始碼裡有、翻譯裡沒有的（漏翻）
  * 翻譯裡有、原始碼裡已經沒有的（多餘）
  * 翻譯裡的 %@、%d 這些記號和原文對不上的（個數或型別不同，執行時會印錯東西甚至當掉）
  * 空的翻譯
  * 「原文#1」這種單數的說法，原文本身卻不在

用法：check_strings.py [--list]      （在哪個目錄執行都可以；--list 只印出原始碼裡的每一句）
有問題時結束碼是 1。建置時 CMake 會跑一次（找得到 python3 的話），有問題就建不過。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES = os.path.join(HERE, '..', 'Sources')
LANGUAGES = ['zh-Hans', 'ja', 'es', 'en']          # zh-Hant 是原文，不需要翻譯表
SPEC = re.compile(r'%(?:(\d+)\$)?([-+0 #]*\d*(?:\.\d+)?)(lld|ld|d|@|f|s|%)')


def unescape(text):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text):
            n = text[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\'}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def source_keys():
    """原始碼裡每一個 L("…") 的原文，以及它出現在哪裡。"""
    keys = {}
    pattern = re.compile(r'(?<![A-Za-z0-9_.])L\("((?:\\.|[^"\\])*)"')
    for name in sorted(os.listdir(SOURCES)):
        if not name.endswith('.swift'):
            continue
        with open(os.path.join(SOURCES, name), encoding='utf-8') as handle:
            for number, line in enumerate(handle, 1):
                if line.lstrip().startswith('//'):
                    continue
                for match in pattern.finditer(line):
                    if '\\(' in match.group(1):
                        print('%s:%d: L() 的原文裡不能用 \\( ) 內插，請改成 %%@ 或 %%d' % (name, number))
                        sys.exit(1)
                    keys.setdefault(unescape(match.group(1)), '%s:%d' % (name, number))
    return keys


def read_strings(path):
    """讀一份 .strings（"原文" = "翻譯"; 一行一句，// 開頭的是註解）。"""
    table = {}
    problems = []
    entry = re.compile(r'^"((?:\\.|[^"\\])*)"\s*=\s*"((?:\\.|[^"\\])*)";\s*$')
    with open(path, encoding='utf-8') as handle:
        for number, line in enumerate(handle, 1):
            text = line.strip()
            if not text or text.startswith('//') or text.startswith('/*'):
                continue
            match = entry.match(text)
            if not match:
                problems.append('第 %d 行看不懂：%s' % (number, text[:60]))
                continue
            key = unescape(match.group(1))
            if key in table:
                problems.append('第 %d 行重複了：%s' % (number, key[:40]))
            table[key] = unescape(match.group(2))
    return table, problems


def placeholders(text):
    """句子裡要填的值，依位置排好的型別（%1$@ 這種指定位置的照它指定的）。"""
    found = []
    position = 0
    for match in SPEC.finditer(text):
        index, _, kind = match.groups()
        if kind == '%':
            continue
        position += 1
        kind = {'lld': 'd', 'ld': 'd'}.get(kind, kind)
        found.append((int(index) if index else position, kind))
    return [kind for _, kind in sorted(found)]


def main():
    keys = source_keys()
    if '--list' in sys.argv:
        for key in keys:
            print(key)
        return 0
    failures = 0
    for language in LANGUAGES:
        path = os.path.join(HERE, language + '.lproj', 'Localizable.strings')
        if not os.path.exists(path):
            print('%s：沒有 %s' % (language, os.path.relpath(path, HERE)))
            failures += 1
            continue
        table, problems = read_strings(path)
        for key, where in keys.items():
            if key not in table:
                problems.append('漏翻（%s）：%s' % (where, key[:70]))
        for key, value in table.items():
            base = key[:-2] if key.endswith('#1') else key
            if base not in keys:
                problems.append('多餘（原始碼裡已經沒有這一句）：%s' % key[:70])
            elif not value.strip() and base.strip():
                problems.append('翻譯是空的：%s' % key[:70])
            elif placeholders(value) != placeholders(base):
                problems.append('要填的值對不上（原文 %s，翻譯 %s）：%s' % (placeholders(base), placeholders(value), key[:60]))
        for problem in problems:
            print('%s：%s' % (language, problem))
        failures += len(problems)
        if not problems:
            print('%s：%d 句都有翻譯' % (language, len(keys)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
