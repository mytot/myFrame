idf_component_register(
    SRCS
        "semver.c"
        "json.c"
        "manifest.c"
        "slot_table.c"
        "base64url.c"
        "sha256.c"
        "canonical.c"
        "sha512.c"
        "ed25519.c"
        "zip.c"
    INCLUDE_DIRS "include"
    # Deliberately no REQUIRES: this component must build for the host as well
    # as for the target, so it may not pull in any ESP-IDF headers.
)
