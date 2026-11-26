"""頁面上的文字（static/i18n.js）：五種語言的鍵與變數要一樣，頁面與程式用到的鍵都要有。

i18n.js 是給瀏覽器跑的，這裡不執行它，只照它固定的寫法把每個語言的字串表讀出來。
"""

from __future__ import annotations

import json
import re
import unittest

from . import util

STATIC = util.WEB_DIR / "static"
LANGUAGES = ("zh-Hant", "zh-Hans", "ja", "es", "en")

_BLOCK = re.compile(r'^    "([A-Za-z-]+)": \{\n(.*?)^    \},$', re.S | re.M)
_ENTRY = re.compile(r'^      "([^"]+)": ("(?:[^"\\]|\\.)*"),$')
_PLACEHOLDER = re.compile(r"\{(\w+)\}")


def load_strings() -> dict[str, dict[str, str]]:
    """每個語言的字串表。讀到看不懂的行就當成錯誤：寧可測試失敗，也不要漏看一行。"""
    source = (STATIC / "i18n.js").read_text(encoding="utf-8")
    tables: dict[str, dict[str, str]] = {}
    for lang, body in _BLOCK.findall(source):
        table: dict[str, str] = {}
        for line in body.splitlines():
            if not line.strip():
                continue
            match = _ENTRY.match(line)
            if match is None:
                raise AssertionError(f"i18n.js 的 {lang} 裡有看不懂的一行：{line!r}")
            if match.group(1) in table:
                raise AssertionError(f"i18n.js 的 {lang} 裡 {match.group(1)} 出現兩次")
            table[match.group(1)] = json.loads(match.group(2))
        tables[lang] = table
    return tables


class StringsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.strings = load_strings()

    def test_the_five_languages_are_there(self):
        self.assertEqual(tuple(self.strings), LANGUAGES)  # 順序也是選單的順序
        source = (STATIC / "i18n.js").read_text(encoding="utf-8")
        menu = re.findall(r'^    \["([A-Za-z-]+)", "[^"]+"\],$', source, re.M)
        self.assertEqual(tuple(menu), LANGUAGES)

    def test_every_language_has_the_same_keys(self):
        reference = self.strings["zh-Hant"]
        self.assertGreater(len(reference), 100)
        for lang in LANGUAGES:
            with self.subTest(lang=lang):
                self.assertEqual(sorted(set(reference) ^ set(self.strings[lang])), [])

    def test_no_text_is_empty_and_placeholders_match(self):
        reference = self.strings["zh-Hant"]
        for lang in LANGUAGES:
            for key, text in self.strings[lang].items():
                with self.subTest(lang=lang, key=key):
                    self.assertTrue(text.strip())
                    self.assertEqual(sorted(_PLACEHOLDER.findall(text)), sorted(_PLACEHOLDER.findall(reference.get(key, ""))))

    def test_translations_are_not_left_in_another_language(self):
        # 簡體、日文、西班牙文、英文的文字不該和繁體中文的一模一樣（只有數字、符號、專有名詞的例外）
        same_everywhere = {"file.meta.unknown"}
        reference = self.strings["zh-Hant"]
        for lang in LANGUAGES[1:]:
            for key, text in self.strings[lang].items():
                if key in same_everywhere or not re.search(r"[一-鿿]", reference[key]):
                    continue
                with self.subTest(lang=lang, key=key):
                    if lang in ("es", "en"):
                        self.assertIsNone(re.search(r"[぀-ヿ一-鿿]", text))  # 沒有漢字或假名
                    elif len(reference[key]) > 12:  # 短的可以剛好一樣，例如日文的「{m} 分 {s} 秒」
                        self.assertNotEqual(text, reference[key])

    def test_keys_used_by_the_pages_exist(self):
        keys = set(self.strings["zh-Hant"])
        for name in ("index.html", "verify.html"):
            html = (STATIC / name).read_text(encoding="utf-8")
            used = set(re.findall(r'data-i18n="([^"]+)"', html))
            self.assertGreater(len(used), 5)
            self.assertEqual(sorted(used - keys), [], name)
            # 語言選單：一個空的 <select>，項目由 i18n.js 填
            self.assertEqual(len(re.findall(r'<select class="lang" id="language"[^>]*hidden></select>', html)), 1, name)
        for name in ("app.js", "verify.js"):
            script = (STATIC / name).read_text(encoding="utf-8")
            used = set(re.findall(r'\bt\("([a-z_.0-9]+)"', script))
            self.assertGreater(len(used), 5)
            # 以「.」結尾的是後面還要接字的開頭（例如 "fail.no_signal." + 名稱），在下一個測試檢查
            self.assertEqual(sorted(key for key in used - keys if not key.endswith(".")), [], name)

    def test_keys_built_at_run_time_exist(self):
        keys = set(self.strings["zh-Hant"])
        # 沒有發卡的原因（rules.py）
        for reason in ("no_signal", "no_callsign", "too_few", "model_mismatch"):
            self.assertIn(f"fail.{reason}.heading", keys)
            self.assertIn(f"fail.{reason}.body", keys)
        for tip in ("tip1", "tip2", "tip3"):
            self.assertIn(f"fail.no_signal.{tip}", keys)
        # 查驗的四種結果（verify.js）
        for kind in ("found", "badsig", "missing", "format"):
            self.assertIn(f"verify.{kind}.heading", keys)
            self.assertIn(f"verify.{kind}.body", keys)
        # 伺服器會回的錯誤代碼
        for code in ("not_wav", "unsupported", "bad_rate", "empty", "too_long", "too_large", "upload_interrupted", "internal",
                     "daily_limit", "expired", "busy", "rate_limited", "already_active", "turnstile_failed",
                     "turnstile_unavailable", "bad_request"):
            self.assertIn(f"error.{code}", keys)

    def test_turnstile_follows_the_page_language(self):
        script = (STATIC / "app.js").read_text(encoding="utf-8")
        for lang in LANGUAGES:
            self.assertRegex(script, rf'"?{re.escape(lang)}"?: "[a-z-]+"')


if __name__ == "__main__":
    unittest.main()
