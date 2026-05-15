#ifndef VERSION_H
#define VERSION_H

/* Individual version components — displayed to the user and in $VER */
#define DEVICE_VERSION 1
#define DEVICE_REVISION 11
#define DEVNAME "virtioscsi.device"

/* Helper macros for stringification */
#define STR(x) #x
#define XSTR(x) STR(x)

/*
 * lib_Version stored in the Resident / Library struct — read by AmigaOS
 * OpenDevice() and by filesystem handlers (SFS 1.290 rejects anything
 * below ~50 as "too old", which is what was silently blocking our mount
 * after the 53.8 → 1.9 renumber).  Keep this pinned to an OS4-era
 * version number so system software accepts us.  DEVVERSIONSTRING below
 * still uses the 1.9 display version the user sees.
 */
#define DEVVER  53
#define DEVREV  DEVICE_REVISION

/*
 * Build date and time: passed from the Makefile via -DBUILD_DATE and
 * -DBUILD_TIME so they reflect the actual build timestamp.  Falls back
 * to the compiler's __DATE__ / __TIME__ if not defined.
 */
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif
#ifndef BUILD_TIME
#define BUILD_TIME __TIME__
#endif

/*
 * Standard AmigaOS version string: $VER: name version.revision (date).
 * MUST use DEVVER/DEVREV (the values stored in lib_Version/lib_Revision)
 * so the numeric version in the string matches what OpenDevice() and
 * any OS4 version-parser reads from the library struct.  Otherwise SFS
 * 1.290 (and other OS4 filesystem handlers) see a mismatch between the
 * numeric lib_Version and the version embedded in the IdString, flag
 * the device as "corrupt / inconsistent", and silently refuse to mount.
 * Display name for users (e.g. GitHub release tag) is v1.9 — that's a
 * README-level concern, not something the library struct reports.
 */
#define DEVVERSIONSTRING DEVNAME " " XSTR(DEVVER) "." XSTR(DEVREV) " (" BUILD_DATE ")"

/*
 * Extended version string for serial debug output at boot.
 * Includes build time for distinguishing debug/release builds.
 */
#define DEVVERSIONSTRING_FULL DEVVERSIONSTRING " [" BUILD_TIME "]"

#endif /* VERSION_H */
