/**
 * @file version_tests.cpp
 * @brief Unit tests for the dynamic version accessors — UfNetCoreLibVersion(),
 *        UfNetCoreLibVersionMajor/Minor/Patch(). Their implementation is emitted
 *        by the generated version.c (src/version.c.sh -> <build>/version.c), so
 *        these tests exercise the output of the version-generation pipeline.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include <version.h>
}

/* Derive the expected values from the SAME macros the pipeline stamps, so the
 * tests remain correct when MAJOR/MINOR/PATCH are bumped in version.h. */
#define STRINGISE_NX(A) #A
#define STRINGISE(A)    STRINGISE_NX(A)

static const char *const kExpectedMajor = STRINGISE(UFNETCORELIB_MAJOR);
static const char *const kExpectedMinor = STRINGISE(UFNETCORELIB_MINOR);
static const char *const kExpectedPatch = STRINGISE(UFNETCORELIB_PATCH);
static const char *const kExpectedFull =
    STRINGISE(UFNETCORELIB_MAJOR) "." STRINGISE(UFNETCORELIB_MINOR) "." STRINGISE(UFNETCORELIB_PATCH);

/* ── Individual components match the header macros ──────────────────────── */

TEST(VersionAccessors, MajorMatchesHeaderMacro) {
  EXPECT_STREQ(UfNetCoreLibVersionMajor(), kExpectedMajor);
}

TEST(VersionAccessors, MinorMatchesHeaderMacro) {
  EXPECT_STREQ(UfNetCoreLibVersionMinor(), kExpectedMinor);
}

TEST(VersionAccessors, PatchMatchesHeaderMacro) {
  EXPECT_STREQ(UfNetCoreLibVersionPatch(), kExpectedPatch);
}

/* ── Composite version string ───────────────────────────────────────────── */

TEST(VersionAccessors, FullEqualsHeaderTriplet) {
  EXPECT_STREQ(UfNetCoreLibVersion(), kExpectedFull);
}

TEST(VersionAccessors, FullIsComponentsJoinedByDots) {
  const std::string composed = std::string(UfNetCoreLibVersionMajor()) + "." +
                               UfNetCoreLibVersionMinor() + "." +
                               UfNetCoreLibVersionPatch();
  EXPECT_EQ(composed, UfNetCoreLibVersion());
}

/* ── Contract: accessors return non-null, non-empty strings ─────────────── */

TEST(VersionAccessors, AccessorsReturnNonEmpty) {
  ASSERT_NE(UfNetCoreLibVersion(), nullptr);
  ASSERT_NE(UfNetCoreLibVersionMajor(), nullptr);
  ASSERT_NE(UfNetCoreLibVersionMinor(), nullptr);
  ASSERT_NE(UfNetCoreLibVersionPatch(), nullptr);
  EXPECT_GT(std::strlen(UfNetCoreLibVersion()), 0u);
  EXPECT_GT(std::strlen(UfNetCoreLibVersionMajor()), 0u);
  EXPECT_GT(std::strlen(UfNetCoreLibVersionMinor()), 0u);
  EXPECT_GT(std::strlen(UfNetCoreLibVersionPatch()), 0u);
}

/* ── Stable across repeated calls (backed by static storage) ────────────── */

TEST(VersionAccessors, StableAcrossCalls) {
  EXPECT_STREQ(UfNetCoreLibVersion(), UfNetCoreLibVersion());
  EXPECT_EQ(UfNetCoreLibVersion(), UfNetCoreLibVersion());  // same backing pointer
}
