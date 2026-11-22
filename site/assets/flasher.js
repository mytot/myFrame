// Generate a manifest for esp-web-tools on the fly — used by flasher page.
// export function is also used in tests.
export function generateManifest({board, tag, assetUrl, erase}) {
  return {
    name: `myFrame — ${board}`,
    version: tag,
    home_assistant_domain: "myframe",
    new_install_skip_erase: !erase,
    builds: [{
      chipFamily: board.includes("s3") || board.includes("sunton") ? "ESP32-S3" : "ESP32",
      parts: [{path: assetUrl, offset: 0}]
    }]
  };
}
