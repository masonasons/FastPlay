#!/usr/bin/env bash
# Archive FastPlay for the iPhone, export it for the App Store, and upload it to
# TestFlight with an App Store Connect API key (the same key and flow as FastSMRW's
# ios/testflight.sh). Needs an App Store Connect app record for me.masonasons.fastplay.
#
#   ios/testflight.sh              archive, export, upload, and release to the public testers
#   ios/testflight.sh --internal   upload for the internal testers only
#   ios/testflight.sh --no-upload  stop after the export (to check that it builds and signs)
#
# The version is the one in include/fastplay/version.h; the build number is the
# count of commits, so each upload's is higher than the last. GitHub runs this
# for every push that touches the iPhone app (.github/workflows/ios-testflight.yml),
# with the key from the repository's secrets.
#
# Before the first run on a machine: ./download-deps.sh, and FFmpeg for iOS
# (ci/ffmpeg/build.sh ios ffmpeg-ios).
set -euo pipefail
cd "$(dirname "$0")"

TEAM="${ASC_TEAM_ID:-9QBYDAX396}"
KEY_ID="${ASC_KEY_ID:-FB9N292RPN}"
ISSUER="${ASC_ISSUER_ID:-02117eeb-7d87-4d4d-bf11-850f80204c4c}"
KEY_PATH="${ASC_KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_${KEY_ID}.p8}"
[ -f "$KEY_PATH" ] || { echo "The App Store Connect key is not at $KEY_PATH" >&2; exit 1; }

VERSION=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' ../include/fastplay/version.h)
BUILD=$(git rev-list --count HEAD)
echo "==> FastPlay $VERSION ($BUILD)"

xcodegen generate

ARCHIVE="build/FastPlay.xcarchive"
EXPORT_DIR="build/export"
AUTH=(-allowProvisioningUpdates -authenticationKeyPath "$KEY_PATH" -authenticationKeyID "$KEY_ID"
      -authenticationKeyIssuerID "$ISSUER")

echo "==> Archiving"
rm -rf "$ARCHIVE"
# Not signed here: the export below signs it for the App Store, with a certificate
# Apple keeps (so a machine with no certificates of its own, GitHub's, can do it).
xcodebuild archive -project FastPlay.xcodeproj -scheme FastPlay \
    -configuration Release -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE" -derivedDataPath build/dd-archive \
    DEVELOPMENT_TEAM="$TEAM" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" \
    MARKETING_VERSION="$VERSION" CURRENT_PROJECT_VERSION="$BUILD"

echo "==> Exporting (App Store)"
rm -rf "$EXPORT_DIR"
xcodebuild -exportArchive -archivePath "$ARCHIVE" -exportPath "$EXPORT_DIR" \
    -exportOptionsPlist exportOptions.plist "${AUTH[@]}"

if [ "${1:-}" = "--no-upload" ]; then
    echo "Exported to $EXPORT_DIR; not uploaded."
    exit 0
fi

echo "==> Uploading to TestFlight"
xcrun altool --upload-app -f "$EXPORT_DIR"/*.ipa -t ios --apiKey "$KEY_ID" --apiIssuer "$ISSUER"

echo "Uploaded. Processing in App Store Connect > TestFlight may take a few minutes."

# Internal testers have it once it is processed. For the public testers it is put
# in their group and sent to beta review, with the last commit as What to Test.
if [ "${1:-}" = "--internal" ]; then
    exit 0
fi
echo "==> Releasing to the public testers"
python3 -m venv build/asc-venv
build/asc-venv/bin/pip install -q pyjwt cryptography
ASC_KEY_ID="$KEY_ID" ASC_ISSUER_ID="$ISSUER" ASC_KEY_PATH="$KEY_PATH" \
    build/asc-venv/bin/python scripts/testflight-public.py "$BUILD" "$(git log -1 --pretty=%B | grep -v -e '^Co-Authored-By:' -e '^Claude-Session:')"
