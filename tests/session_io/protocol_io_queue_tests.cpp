#include <gtest/gtest.h>
extern "C" void ProtocolIoTestEofLockOrder(void);
TEST(ProtocolIoQueue, ReleasesInboundLockBeforeSuspendingOnEof) { ProtocolIoTestEofLockOrder(); }
extern "C" void ProtocolIoTestBusyQueue(void);
TEST(ProtocolIoQueue, BleedsDuringServiceAndConsolidatesEveryByte) { ProtocolIoTestBusyQueue(); }
