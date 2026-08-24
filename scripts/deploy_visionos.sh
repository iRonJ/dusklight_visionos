#!/usr/bin/env bash
set -e

DEVICE_ID="${1:-${VISIONOS_DEVICE_ID:-18CDEAA4-EF77-51EF-A3DF-ED311A27E277}}"
BUILD_DIR="${2:-${VISIONOS_BUILD_DIR:-build/visionos-default}}"
APP_BUNDLE="${BUILD_DIR}/Dusklight.app"
ENTITLEMENTS="/tmp/dusk_entitlements.plist"
DEFAULT_SIGNING_IDENTITY="Apple Development: Ron Jailall (B2GK4YK2KT)"
PROVISION_PROFILE="${VISIONOS_PROVISIONING_PROFILE:-${HOME}/Library/Developer/Xcode/UserData/Provisioning Profiles/8370c7cf-8481-4fff-b94a-93fee757c56f.mobileprovision}"
SIGNING_IDENTITY="${VISIONOS_SIGNING_IDENTITY:-${DEFAULT_SIGNING_IDENTITY}}"
TEAM_ID="${VISIONOS_TEAM_ID:-4Q4UKUH48B}"

echo "==> Building Dusklight with Ninja..."
ninja -C "${BUILD_DIR}" dusklight

echo "==> Preparing Entitlements and Provisioning Profile..."
cat << EOP > "${ENTITLEMENTS}"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>application-identifier</key>
    <string>${TEAM_ID}.dev.twilitrealm.dusk</string>
    <key>com.apple.developer.team-identifier</key>
    <string>${TEAM_ID}</string>
    <key>get-task-allow</key>
    <true/>
</dict>
</plist>
EOP

if [ ! -f "${PROVISION_PROFILE}" ]; then
    echo "Provisioning profile not found: ${PROVISION_PROFILE}" >&2
    echo "Set VISIONOS_PROVISIONING_PROFILE to a profile for dev.twilitrealm.dusk." >&2
    exit 1
fi
cp "${PROVISION_PROFILE}" "${APP_BUNDLE}/embedded.mobileprovision"

echo "==> Codesigning Dusklight.app..."
codesign --force --generate-entitlement-der --sign "${SIGNING_IDENTITY}" --entitlements "${ENTITLEMENTS}" "${APP_BUNDLE}"

echo "==> Installing Dusklight onto Apple Vision Pro (${DEVICE_ID})..."
xcrun devicectl device install app --device "${DEVICE_ID}" "${APP_BUNDLE}"

echo "==> Deployment Complete! Launch Dusklight from the Vision Pro Home View."
