#!/usr/bin/env bash
# 把建置好的 NBTV.app 做成「下載之後不會被 macOS 擋」的版本：
#   1. 用 Developer ID 憑證簽章（開 Hardened Runtime、帶安全時間戳記，權限宣告在 NBTV.entitlements：鏡頭與音訊輸入）
#   2. 壓縮後送 Apple 公證，等它回覆
#   3. 把公證的票據釘在 app 上（沒有網路時 Gatekeeper 也驗得過）
#   4. 重新壓成發佈用的 zip
# 建置本身（apps/macos/CMakeLists.txt）做的仍然是 ad-hoc 簽章；要發佈時才用這支指令稿把它換掉。
# 原來的 app 不會被改到：簽的是複製出來的那一份。
#
#   apps/macos/sign-and-notarize.sh <NBTV.app> <輸出的.zip>
#
# 用環境變數指定身分：
#   NBTV_SIGN_IDENTITY     簽章身分：「Developer ID Application: …」的全名或它的 SHA-1（security find-identity -v -p codesigning 列得出來）
#   NBTV_SIGN_KEYCHAIN     （可以不給）身分所在的鑰匙圈；CI 用暫時的鑰匙圈時要給
# 公證的登入方式三選一：
#   NBTV_NOTARY_PROFILE    用 xcrun notarytool store-credentials 存在鑰匙圈裡的設定檔名稱（自己的電腦上用這個）
#   NBTV_NOTARY_KEY        App Store Connect API 金鑰（.p8 檔的路徑），加上 NBTV_NOTARY_KEY_ID；
#                          團隊金鑰還要 NBTV_NOTARY_ISSUER，個人金鑰不給
#   NBTV_NOTARY_APPLE_ID   Apple ID，加上 NBTV_NOTARY_TEAM_ID 與 NBTV_NOTARY_PASSWORD（App 專用密碼，不是帳號的密碼）
#   NBTV_NOTARY_TIMEOUT    （可以不給）等 Apple 回覆最多多久，預設 30m
#
# 這支指令稿不會把簽章身分的名稱印出來（CI 的紀錄是公開的）；簽好的 app 裡當然看得到。
# bundle 裡現在只有一個可執行檔。以後如果加了內嵌的程式或程式庫，要先由內而外各自簽過，這裡要跟著改。
set -euo pipefail

die() {
    echo "不行：$*" >&2
    exit 1
}

[ $# -eq 2 ] || { echo "用法：$0 <NBTV.app> <輸出的.zip>" >&2; exit 2; }
app="${1%/}"
out="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
entitlements="$here/NBTV.entitlements"

[ -d "$app" ] && [ -f "$app/Contents/Info.plist" ] || die "$app 不是 app bundle"
[ -f "$entitlements" ] || die "找不到 $entitlements"
case "$out" in *.zip) ;; *) die "輸出的檔名要以 .zip 結尾：$out" ;; esac

identity="${NBTV_SIGN_IDENTITY:-}"
[ -n "$identity" ] || die "沒有設定 NBTV_SIGN_IDENTITY（簽章身分）"
[ "$identity" != "-" ] || die "ad-hoc 簽章（-）不能公證，要用 Developer ID Application 的身分"

# 公證的登入參數。先把這一步檢查完再動手簽，免得簽了才發現沒辦法送審。
notary=()
if [ -n "${NBTV_NOTARY_PROFILE:-}" ]; then
    notary=(--keychain-profile "$NBTV_NOTARY_PROFILE")
elif [ -n "${NBTV_NOTARY_KEY:-}" ]; then
    [ -f "$NBTV_NOTARY_KEY" ] || die "NBTV_NOTARY_KEY 指的檔案不存在"
    [ -n "${NBTV_NOTARY_KEY_ID:-}" ] || die "用 API 金鑰要一起給 NBTV_NOTARY_KEY_ID"
    notary=(--key "$NBTV_NOTARY_KEY" --key-id "$NBTV_NOTARY_KEY_ID")
    if [ -n "${NBTV_NOTARY_ISSUER:-}" ]; then notary+=(--issuer "$NBTV_NOTARY_ISSUER"); fi
elif [ -n "${NBTV_NOTARY_APPLE_ID:-}" ]; then
    [ -n "${NBTV_NOTARY_TEAM_ID:-}" ] && [ -n "${NBTV_NOTARY_PASSWORD:-}" ] || die "用 Apple ID 要一起給 NBTV_NOTARY_TEAM_ID 與 NBTV_NOTARY_PASSWORD"
    notary=(--apple-id "$NBTV_NOTARY_APPLE_ID" --team-id "$NBTV_NOTARY_TEAM_ID" --password "$NBTV_NOTARY_PASSWORD")
else
    die "沒有設定公證的登入方式（NBTV_NOTARY_PROFILE、NBTV_NOTARY_KEY 或 NBTV_NOTARY_APPLE_ID）"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
name="$(basename "$app")"
copy="$work/$name"
ditto "$app" "$copy"

echo "簽章：$name"
sign=(--force --options runtime --timestamp --entitlements "$entitlements" --sign "$identity")
if [ -n "${NBTV_SIGN_KEYCHAIN:-}" ]; then sign+=(--keychain "$NBTV_SIGN_KEYCHAIN"); fi
# codesign 會印「replacing existing signature」；身分找不到或鑰匙圈不讓用時會在這裡失敗
codesign "${sign[@]}" "$copy"
codesign --verify --deep --strict "$copy"

# 簽出來的要是公證收的那一種：Developer ID、Hardened Runtime、有時間戳記。不是的話不必送審，一定被退。
details="$(codesign --display --verbose=4 "$copy" 2>&1)"
grep -q "^Authority=Developer ID Application: " <<< "$details" || die "簽章身分不是 Developer ID Application（公證只收這一種）"
grep -E -q "^CodeDirectory .*flags=[^ ]*runtime" <<< "$details" || die "簽章沒有開 Hardened Runtime"
grep -q "^Timestamp=" <<< "$details" || die "簽章沒有安全時間戳記"
grep -E "^(Identifier|Format|Timestamp)=" <<< "$details"

echo "送審：壓縮並上傳給 Apple"
ditto -c -k --keepParent "$copy" "$work/upload.zip"
if ! xcrun notarytool submit "$work/upload.zip" "${notary[@]}" --wait --timeout "${NBTV_NOTARY_TIMEOUT:-30m}" \
    --no-progress --output-format json > "$work/submit.json" 2> "$work/submit.err"; then
    cat "$work/submit.err" >&2
    cat "$work/submit.json" >&2 || true
    die "送審沒有完成"
fi
status="$(plutil -extract status raw -o - "$work/submit.json" 2> /dev/null || true)"
id="$(plutil -extract id raw -o - "$work/submit.json" 2> /dev/null || true)"
echo "送審編號 ${id:-（沒有）}：${status:-（沒有結果）}"
# 審查紀錄不管過不過都看：通過了也可能有警告
if [ -n "$id" ] && xcrun notarytool log "$id" "${notary[@]}" "$work/log.json" > /dev/null 2>&1; then
    issues="$(plutil -extract issues json -o - "$work/log.json" 2> /dev/null || echo null)"
    if [ "$issues" != "null" ] && [ "$issues" != "[]" ]; then
        echo "審查紀錄裡的問題與警告：" >&2
        echo "$issues" >&2
    fi
fi
[ "$status" = "Accepted" ] || die "Apple 沒有接受（$status）"

echo "釘上票據"
xcrun stapler staple -q "$copy"
xcrun stapler validate -q "$copy"
codesign --verify --deep --strict "$copy"

# Gatekeeper 的評估。有些機器（例如 CI）把評估整個關掉了，那種機器上只印出來；開著的話就要放行才算數。
assessment="$(spctl --assess --type execute -vv "$copy" 2>&1 || true)"
grep -v "^origin=" <<< "$assessment" | sed "s|^$copy|$name|"
gatekeeper="$(spctl --status 2>&1 || true)"
if grep -q "enabled" <<< "$gatekeeper"; then
    grep -q "source=Notarized Developer ID" <<< "$assessment" || die "Gatekeeper 沒有放行"
fi

mkdir -p "$(dirname "$out")"
rm -f "$out"
ditto -c -k --keepParent "$copy" "$out"
echo "完成：$out（$(($(stat -f %z "$out") / 1048576)) MB）"
