#!/usr/bin/env bash
# Check whether the newest upstream versions of doctest, esp-dsp, esp-mqtt,
# esp_gcov and esp-idf are newer than the pinned versions vendored in this
# repository.
#
# esp-idf is tracked per minor release: the newest non-prerelease version of
# each pinned minor is compared against its pinned version, and releases of
# minor versions that are not tracked at all are reported (old minor versions
# that are no longer tracked are fine).
#
# Exits with status 1 if any upstream version is newer than the pinned one.
set -euo pipefail

# Pinned versions currently vendored in this repository.
PINNED_DOCTEST="v2.5.3"
PINNED_ESP_DSP="v1.8.2"
PINNED_ESP_MQTT="v1.1.0"
PINNED_ESP_GCOV="1.0.5"
# Pinned esp-idf releases, one per tracked minor version.
PINNED_ESP_IDF_MINORS=(
    "6.0:v6.0.2"
    "6.1:v6.1"
)

# Fetch the newest non-prerelease version tag of a GitHub repository. Uses the
# /releases endpoint and falls back to the latest tag when the repository
# publishes no GitHub releases (esp-dsp).
latest_tag() {
    local repo="$1" tag
    tag="$(curl -fsSL "https://api.github.com/repos/$repo/releases" |
        jq -r '[.[] | select(.prerelease == false and .draft == false) | .tag_name][0] // empty')"
    if [[ -z "$tag" ]]; then
        tag="$(curl -fsSL "https://api.github.com/repos/$repo/tags" | jq -r '.[0].name // empty')"
    fi
    printf '%s' "$tag"
}

# Fetch the newest non-prerelease version tag of a GitHub repository that
# belongs to the given minor release (e.g. "6.0" matches v6.0.2 and the
# initial "6.0" release without a patch field).
latest_tag_of_minor() {
    local repo="$1" minor="$2"
    curl -fsSL "https://api.github.com/repos/$repo/releases" |
        jq -r --arg minor "v$minor" \
            '[.[] | select(.prerelease == false and .draft == false) | .tag_name | select(. == $minor or startswith($minor + "."))][0] // empty'
}

# Fetch the minor releases of a GitHub repository that have a non-prerelease
# version, newest first (e.g. "6.0", "5.5", "5.2").
minor_releases() {
    local repo="$1"
    curl -fsSL "https://api.github.com/repos/$repo/releases" |
        jq -r '[.[] | select(.prerelease == false and .draft == false) | .tag_name | capture("^v(?<m>[0-9]+\\.[0-9]+)\\.?").m] | unique | reverse[]'
}

check() {
    local name="$1" pinned="$2" repo="$3" latest
    latest="$(latest_tag "$repo")"
    if [[ -z "$latest" ]]; then
        echo "$name: could not determine latest upstream version" >&2
        return 1
    fi
    if [[ "$(printf '%s\n%s\n' "$pinned" "$latest" | sort -V | tail -1)" != "$pinned" ]]; then
        echo "$name: upstream $latest is newer than pinned $pinned"
        return 1
    fi
}

check_minor() {
    local repo="$1" minor="$2" pinned="$3" latest
    latest="$(latest_tag_of_minor "$repo" "$minor")"
    if [[ -z "$latest" ]]; then
        echo "esp-idf $minor: could not determine latest upstream version" >&2
        return 1
    fi
    if [[ "$(printf '%s\n%s\n' "$pinned" "$latest" | sort -V | tail -1)" != "$pinned" ]]; then
        echo "esp-idf $minor: upstream $latest is newer than pinned $pinned"
        return 1
    fi
}

# Fetch the version field from the idf_component.yml of an espressif
# idf-extra-components component.
latest_extra_component_version() {
    local component="$1"
    curl -fsSL "https://raw.githubusercontent.com/espressif/idf-extra-components/refs/heads/master/$component/idf_component.yml" |
        awk -F': *' '/^version:/ {gsub(/"/, "", $2); print $2; exit}'
}

check_untracked_minors() {
    local repo="$1" minor newest_tracked="0.0"
    for entry in "${PINNED_ESP_IDF_MINORS[@]}"; do
        if [[ "$(printf '%s\n%s\n' "$newest_tracked" "${entry%%:*}" | sort -V | tail -1)" != "$newest_tracked" ]]; then
            newest_tracked="${entry%%:*}"
        fi
    done
    for minor in $(minor_releases "$repo"); do
        # Only newer minor releases need to be tracked; it is fine if old
        # minor releases are no longer tracked.
        if [[ "$(printf '%s\n%s\n' "$newest_tracked" "$minor" | sort -V | tail -1)" != "$newest_tracked" ]]; then
            echo "esp-idf: untracked minor release $minor exists upstream"
            return 1
        fi
    done
}

status=0
check "doctest" "$PINNED_DOCTEST" "doctest/doctest" || status=1
check "esp-dsp" "$PINNED_ESP_DSP" "espressif/esp-dsp" || status=1
check "esp-mqtt" "$PINNED_ESP_MQTT" "espressif/esp-mqtt" || status=1
if ! latest="$(latest_extra_component_version "esp_gcov")"; then
    echo "esp_gcov: could not determine latest upstream version" >&2
    status=1
elif [[ -z "$latest" ]]; then
    echo "esp_gcov: could not determine latest upstream version" >&2
    status=1
elif [[ "$(printf '%s\n%s\n' "$PINNED_ESP_GCOV" "$latest" | sort -V | tail -1)" != "$PINNED_ESP_GCOV" ]]; then
    echo "esp_gcov: upstream $latest is newer than pinned $PINNED_ESP_GCOV"
    status=1
fi
for entry in "${PINNED_ESP_IDF_MINORS[@]}"; do
    check_minor "espressif/esp-idf" "${entry%%:*}" "${entry##*:}" || status=1
done
check_untracked_minors "espressif/esp-idf" || status=1
echo "Checked all upstream projects."
exit "$status"
