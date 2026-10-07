from pathlib import Path
import hashlib

PATCHES = (
    (
        "src/common/FsCache.cpp",
        "ba4f99dd660c7c6a747b20abcf11354379aa689115d1ee1ad1cf9577706a67bb",
        "f46a551f97c674ab00c9c25385af7c370a776725f17e14502fcc357858f583da",
        """      if (!m_blockDev->readSector(sector, m_buffer)) {
        DBG_FAIL_MACRO;
        goto fail;
      }""",
        """      if (!m_blockDev->readSector(sector, m_buffer)) {
        // A failed transfer may have overwritten the old cached sector.
        invalidate();
        DBG_FAIL_MACRO;
        goto fail;
      }""",
    ),
    (
        "src/SdFatConfig.h",
        "7889975cad262158e1623c873730210c41c55c2198830d668952b82e588ff7cc",
        "32104db82acc857b70c7fe20740afbffa9f7dee0a7c685c106febfa989fdbe77",
        """ * for FAT16/FAT32 table entries.  This improves performance for large
 * writes that are not a multiple of 512 bytes.
 */
#ifdef __arm__
#define USE_SEPARATE_FAT_CACHE 1
#else  // __arm__
#define USE_SEPARATE_FAT_CACHE 0
#endif  // __arm__""",
        """ * for FAT16/FAT32 table entries.  This improves performance for large
 * writes that are not a multiple of 512 bytes.
 */
#ifndef USE_SEPARATE_FAT_CACHE
#ifdef __arm__
#define USE_SEPARATE_FAT_CACHE 1
#else  // __arm__
#define USE_SEPARATE_FAT_CACHE 0
#endif  // __arm__
#endif  // USE_SEPARATE_FAT_CACHE""",
    ),
)

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False

def apply_patches(project_dir, dependency_dir):
    project = Path(project_dir).resolve()
    dependency = Path(dependency_dir).resolve()

    libdeps_root = (project / ".pio" / "libdeps").resolve()
    if not inside(dependency, libdeps_root):
        raise RuntimeError("SdFat must be inside this project's .pio/libdeps")

    properties = dependency / "library.properties"
    if not properties.is_file():
        raise RuntimeError("Missing SdFat library.properties")

    version_lines = properties.read_text(encoding="utf-8").splitlines()
    if "version=2.3.1" not in version_lines:
        raise RuntimeError("SdFat patches require version 2.3.1")

    for relative, upstream_hash, patched_hash, old, new in PATCHES:
        target = dependency / relative

        if not target.is_file() or not inside(target.resolve(), dependency):
            raise RuntimeError("Missing or invalid SdFat target: " + relative)

        current_hash = sha256(target)

        if current_hash == patched_hash:
            print("SdFat patch already applied: " + relative)
            continue

        if current_hash != upstream_hash:
            raise RuntimeError(
                "Unrecognized SdFat source: " + relative +
                " (SHA256 " + current_hash + ")"
            )

        raw = target.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise RuntimeError("SdFat source is not UTF-8: " + relative) from exc

        if old not in text:
            raise RuntimeError("Expected SdFat source text not found: " + relative)

        text = text.replace(old, new, 1)
        target.write_bytes(text.encode("utf-8"))

        final_hash = sha256(target)
        if final_hash != patched_hash:
            raise RuntimeError(
                "Unexpected patched SdFat source: " + relative +
                " (SHA256 " + final_hash + ")"
            )

        print("Applied SdFat patch: " + relative)

def patch_selected_dependency(env):
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return

    selected = [
        builder for builder in env.GetLibBuilders()
        if builder.name == "SdFat" and builder.is_dependent
    ]

    if len(selected) != 1:
        raise RuntimeError("Expected exactly one selected SdFat dependency")

    dependency = Path(selected[0].path).resolve()
    environment_root = (
        Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    ).resolve()

    if not inside(dependency, environment_root):
        raise RuntimeError("SdFat is outside the selected environment's dependencies")

    apply_patches(env["PROJECT_DIR"], dependency)

if "Import" in globals():
    Import("env")  # noqa: F821
    patch_selected_dependency(env)  # noqa: F821
