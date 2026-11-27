#include <gtest/gtest.h>
extern "C" void SessionIoPublicationTestUnlockWindow(void);
extern "C" void SessionIoPublicationTestNoAppend(void);
TEST(SessionIoPublication, UnlockWindowPreservesEdgesAndWakesOneWaiter) { SessionIoPublicationTestUnlockWindow(); }
TEST(SessionIoPublication, NoAppendDoesNotSignalOrRetainClaim) { SessionIoPublicationTestNoAppend(); }
