"""Signs the Windows archive of a draft release and uploads <archive>.sig.

Usage: GH_TOKEN=... python sign_release.py <version> <private key .pem>

The client installs a release only with a matching Ed25519 signature; the
key lives off GitHub, so this runs on the releaser's machine, after the
draft is made (Actions -> Windows -> Run workflow) and before Publish.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import urllib.request

REPO = "fucksynth/fuckgram"


def api(method, url, body=None, content_type="application/json", accept="application/vnd.github+json"):
    request = urllib.request.Request(url, method=method, data=body, headers={
        "Authorization": "Bearer " + os.environ["GH_TOKEN"],
        "Accept": accept,
        "Content-Type": content_type,
        "User-Agent": "fuckgram-release-sign",
    })
    with urllib.request.urlopen(request) as reply:
        return reply.read()


def main():
    version, key = sys.argv[1], sys.argv[2]
    releases = json.loads(api("GET", f"https://api.github.com/repos/{REPO}/releases?per_page=20"))
    release = next((r for r in releases if r["tag_name"] == version), None)
    if not release:
        sys.exit(f"No release {version}.")
    if not release["draft"]:
        sys.exit(f"{version} is published already: sign drafts only.")
    archive = next((a for a in release["assets"]
                    if a["name"].endswith(".zip") and "win" in a["name"] and "x64" in a["name"]), None)
    if not archive:
        sys.exit("The draft has no win-x64 zip.")
    signature_name = archive["name"] + ".sig"
    for asset in release["assets"]:
        if asset["name"] == signature_name:
            api("DELETE", asset["url"])
            print("Removed the old", signature_name)

    data = api("GET", archive["url"], accept="application/octet-stream")
    if len(data) != archive["size"]:
        sys.exit("The downloaded archive has a wrong size.")
    folder = tempfile.mkdtemp()
    data_path = os.path.join(folder, "archive.zip")
    sig_path = os.path.join(folder, "archive.sig")
    try:
        with open(data_path, "wb") as f:
            f.write(data)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key,
                        "-in", data_path, "-out", sig_path], check=True)
        # Check it against the public half before it goes out.
        public = subprocess.run(["openssl", "pkey", "-in", key, "-pubout"],
                                check=True, capture_output=True).stdout
        public_path = os.path.join(folder, "public.pem")
        with open(public_path, "wb") as f:
            f.write(public)
        subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", public_path,
                        "-rawin", "-in", data_path, "-sigfile", sig_path],
                       check=True, capture_output=True)
        signature = open(sig_path, "rb").read()
    finally:
        for name in os.listdir(folder):
            os.remove(os.path.join(folder, name))
        os.rmdir(folder)
    if len(signature) != 64:
        sys.exit("Unexpected signature size.")

    upload = release["upload_url"].split("{")[0] + "?name=" + signature_name
    api("POST", upload, (base64.b64encode(signature).decode() + "\n").encode(),
        content_type="text/plain")
    print(f"Signed {archive['name']} ({len(data)} bytes): {signature_name} uploaded to the draft {version}.")


if __name__ == "__main__":
    main()
