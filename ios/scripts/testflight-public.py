#!/usr/bin/env python3
"""Gives an uploaded build to TestFlight's public testers.

    ios/scripts/testflight-public.py <build number> [what to test]   (needs PyJWT, cryptography)

Internal testers get every build as it is processed. Public ones get a build only
once it is put in their group and sent to Apple's beta review, which this does:
it waits for Apple to finish processing the build, sets its "What to Test" text,
adds it to the group named Public and submits it for review (which later builds
of a version that has passed once usually clear at once).

The App Store Connect key comes from ASC_KEY_ID, ASC_ISSUER_ID and ASC_KEY_PATH,
as in testflight.sh, which runs this after an upload.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import jwt

BUNDLE_ID = "me.masonasons.fastplay"
GROUP = "Public"
KEY_ID = os.environ["ASC_KEY_ID"]
ISSUER = os.environ["ASC_ISSUER_ID"]
KEY = open(os.environ["ASC_KEY_PATH"]).read()


def call(method, path, body=None):
    # A token for each call: waiting for processing can outlast one
    now = int(time.time())
    token = jwt.encode({"iss": ISSUER, "iat": now, "exp": now + 600, "aud": "appstoreconnect-v1"}, KEY,
                       algorithm="ES256", headers={"kid": KEY_ID})
    request = urllib.request.Request(
        "https://api.appstoreconnect.apple.com" + path, method=method,
        data=json.dumps(body).encode() if body else None,
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"{}")


def problems(result):
    return "; ".join(e.get("detail") or e.get("title") or "?" for e in result.get("errors", []))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    number = sys.argv[1]
    notes = (sys.argv[2] if len(sys.argv) > 2 else "").strip() or "Fixes and improvements."
    notes = notes[:4000]

    status, apps = call("GET", "/v1/apps?filter[bundleId]=" + BUNDLE_ID)
    app = next((a["id"] for a in apps.get("data", []) if a["attributes"]["bundleId"] == BUNDLE_ID), None)
    if not app:
        sys.exit(f"No app {BUNDLE_ID} on App Store Connect ({status} {problems(apps)})")

    # Apple takes a few minutes to process an upload before it can go anywhere
    build = None
    deadline = time.time() + 45 * 60
    while time.time() < deadline:
        status, builds = call("GET", f"/v1/builds?filter[app]={app}&filter[version]={number}")
        found = builds.get("data", [])
        state = found[0]["attributes"]["processingState"] if found else "not listed yet"
        if state == "VALID":
            build = found[0]["id"]
            break
        if state in ("FAILED", "INVALID"):
            sys.exit(f"Apple could not process build {number}: {state}")
        print(f"build {number}: {state}", flush=True)
        time.sleep(30)
    if not build:
        sys.exit(f"Build {number} was still not processed after 45 minutes")

    # What to Test, which a build for public testers must have
    status, texts = call("GET", f"/v1/builds/{build}/betaBuildLocalizations")
    if texts.get("data"):
        text = texts["data"][0]["id"]
        status, result = call("PATCH", f"/v1/betaBuildLocalizations/{text}", {"data": {
            "type": "betaBuildLocalizations", "id": text, "attributes": {"whatsNew": notes}}})
    else:
        status, result = call("POST", "/v1/betaBuildLocalizations", {"data": {
            "type": "betaBuildLocalizations", "attributes": {"locale": "en-US", "whatsNew": notes},
            "relationships": {"build": {"data": {"type": "builds", "id": build}}}}})
    if status >= 300:
        sys.exit(f"Could not set What to Test: {status} {problems(result)}")

    status, groups = call("GET", f"/v1/apps/{app}/betaGroups")
    group = next((g["id"] for g in groups.get("data", [])
                  if g["attributes"]["name"] == GROUP and not g["attributes"]["isInternalGroup"]), None)
    if not group:
        sys.exit(f"No public tester group named {GROUP}")
    status, result = call("POST", f"/v1/betaGroups/{group}/relationships/builds",
                          {"data": [{"type": "builds", "id": build}]})
    if status >= 300:
        sys.exit(f"Could not add the build to {GROUP}: {status} {problems(result)}")

    status, result = call("POST", "/v1/betaAppReviewSubmissions", {"data": {
        "type": "betaAppReviewSubmissions",
        "relationships": {"build": {"data": {"type": "builds", "id": build}}}}})
    if status < 300:
        print(f"build {number}: in {GROUP}, review {result['data']['attributes']['betaReviewState']}")
    elif status == 409:  # already submitted, or already through
        print(f"build {number}: in {GROUP} ({problems(result)})")
    else:
        sys.exit(f"Could not submit for beta review: {status} {problems(result)}")


if __name__ == "__main__":
    main()
