#include <gtest/gtest.h>

extern "C" {
void SessionIoTestExplicitWriteRetry(void);
void SessionIoTestRecoverableRead(void);
void SessionIoTestDeferredTerminalRead(void);
void SessionIoTestReadClassification(void);
void SessionIoTestContinuations(void);
void SessionIoTestConcurrent(void);
void SessionIoTestPressure(void);
void SessionIoTestStatusFlags(void);
void SessionIoTestDeferral(void);
void SessionIoTestBusyBleed(void);
void SessionIoTestOwnerKeepsLaterEdges(void);
void SessionIoTestPublishAndHandoff(void);
void SessionIoTestIdleHandoffGeneration(void);
void SessionIoTestCompletionOwnership(void);
void SessionIoTestSocketGenerationReuse(void);
void SessionIoTestGenerationHandoffAndCompletion(void);
void SessionIoTestSocketDispatch(void);
void SessionIoTestTerminalDrain(void);
void SessionIoTestGenerationAndCancel(void);
}

TEST(SessionIoWork, NoAllocationOrCapacityLoss) { SessionIoTestPressure(); }
TEST(SessionIoWork, BusyAndSuspendedDoNotBlockOtherSessions) { SessionIoTestDeferral(); }
TEST(SessionIoWork, BusyReadKeepsTheWriteBit) { SessionIoTestBusyBleed(); }
TEST(SessionIoWork, OwnerKeepsLaterEdges) { SessionIoTestOwnerKeepsLaterEdges(); }
TEST(SessionIoWork, PublishKeepsOneReferenceAndHandoffRejectsQueuedDestination) { SessionIoTestPublishAndHandoff(); }
TEST(SessionIoWork, IdleHandoffAcceptsStaleIdentityAndPreservesForeignWork) { SessionIoTestIdleHandoffGeneration(); }
TEST(SessionIoWork, CompletionValidatesOwnershipAndAbandonsOnShutdown) { SessionIoTestCompletionOwnership(); }
TEST(SessionIoWork, KernelReadinessAndBackpressure) { SessionIoTestSocketDispatch(); }
TEST(SessionIoWork, TerminalReadsDrainToEofWithoutWriting) { SessionIoTestTerminalDrain(); }
TEST(SessionIoWork, GenerationAndShutdownReferences) { SessionIoTestGenerationAndCancel(); }

TEST(SessionIoWork, ConcurrentEpollDelivery) { SessionIoTestConcurrent(); }

TEST(SessionIoWork, IndependentReadAndWriteContinuations) { SessionIoTestContinuations(); }

TEST(SessionIoWork, ExplicitWritableRetry) { SessionIoTestExplicitWriteRetry(); }

TEST(SessionIoWork, RecoverableReadRetainsBothEdges) { SessionIoTestRecoverableRead(); }
TEST(SessionIoWork, DeferredTerminalReadNeverWrites) { SessionIoTestDeferredTerminalRead(); }
TEST(SessionIoWork, ClassifiesRealServerResults) { SessionIoTestReadClassification(); }

TEST(SessionIoWork, ActualSessionStatusMasks) { SessionIoTestStatusFlags(); }

TEST(SessionIoWork, SocketGenerationRejectsSameCidFdStaleTerminalEdges) { SessionIoTestSocketGenerationReuse(); }
TEST(SessionIoWork, HandoffAndCompletionRejectPreviousSocketGeneration) { SessionIoTestGenerationHandoffAndCompletion(); }
