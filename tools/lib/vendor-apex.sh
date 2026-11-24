#!/usr/bin/env bash
# Common outside-Soong packing/signing steps for MatonOS vendor APEXes.
set -Eeuo pipefail
declare -a VENDOR_APEX_PREPARE_ARGS

vendor_apex_finish() {
  local name=$1 payload=$2 file_contexts=$3 vintf=$4 version=$5 out_dir=$6 key_dir=$7 tmp=$8
  local device_dir=$9 aosp=${10} product_out=${11} hostbin=${12} hostlib=${13} readelf=${14}
  local payload_key=$key_dir/$name.pem payload_pub=$key_dir/$name.avbpubkey
  export APEXER_TOOL_PATH="$hostbin:$hostlib:$aosp/prebuilts/sdk/tools/linux/bin"
  python3 "$device_dir/tools/lib/vendor-apex.py" prepare \
    --payload "$payload" --product-out "$product_out" --temp "$tmp" --name "$name" \
    --version "$version" --readelf "$readelf" "${VENDOR_APEX_PREPARE_ARGS[@]}"
  "$hostbin/avbtool" extract_public_key --key "$payload_key" --output "$payload_pub"
  "$hostbin/conv_apex_manifest" proto "$tmp/apex_manifest.json" -o "$tmp/apex_manifest.pb"
  "$hostbin/apexer" --force --do_not_check_keyname \
    --manifest "$tmp/apex_manifest.pb" --manifest_json "$tmp/apex_manifest.json" \
    --file_contexts "$file_contexts" --canned_fs_config "$tmp/canned_fs_config" \
    --key "$payload_key" --pubkey "$payload_pub" --android_manifest "$tmp/AndroidManifest.xml" \
    --android_jar_path "$aosp/prebuilts/sdk/current/public/android.jar" \
    --apexer_tool_path "$APEXER_TOOL_PATH" "$payload" "$tmp/$name.apex.unsigned"
  java -Djava.library.path="$hostlib" -jar "$aosp/out/host/linux-x86/framework/signapk.jar" \
    -a 4096 --align-file-size --disable-v1 "$key_dir/releasekey.x509.pem" "$key_dir/releasekey.pk8" \
    "$tmp/$name.apex.unsigned" "$out_dir/$name.apex"
  python3 "$device_dir/tools/lib/vendor-apex.py" compat \
    --payload "$payload" --product-out "$product_out" --vintf "$vintf" \
    --name "$name" --version "$version" --readelf "$readelf" \
    --output "$out_dir/$name.compat.json"
  echo "Built $out_dir/$name.apex (version $version, payload key $payload_key)"
  echo "Wrote compatibility record $out_dir/$name.compat.json"
}
