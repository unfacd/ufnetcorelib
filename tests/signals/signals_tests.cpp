/**
 * @file signals_tests.cpp
 * @brief Adversarial tests for the signal-handling design (Stage 1).
 *
 * Exercises the real InitSignals() and UfsrvSignalControlSet() from
 * src/ufnet_msgcore/misc.c:
 *   - mask asserts (§2): the control signals are blocked, SIGPIPE is ignored,
 *     SIGUSR1/SIGUSR2 have stub handlers, and the getter matches the block.
 *   - a signalfd+select harness (§1) that wakes on a real signal delivery.
 *   - adversarial cases (§3/§4/§5): worker inheritance, standard-signal
 *     coalescing under a 100× flood, and worker exclusion.
 */

#include <gtest/gtest.h>

#include <signal.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/signalfd.h>
#include <sys/select.h>
#include <unistd.h>

#include <cerrno>

extern "C" {
/* Forward-declared rather than including <ufnetcorelib/ufnet_msgcore/misc.h>,
 * which transitively pulls in lua / json-c / uflib headers that require
 * -fblocks (a C-only flag in this build). The symbols come from misc.c. */
void InitSignals(void);
const sigset_t *UfsrvSignalControlSet(void);
}

namespace {

/* Drain any pending signalfd events so a later assert sees only new ones. */
void DrainSignalfd(int sfd) {
  struct signalfd_siginfo si;
  while (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
  }
}

/* A worker that reports whether the control signals are still blocked in its
 * own mask — this is the "block-before-pthread_create" inheritance claim. */
struct WorkerProbe {
  int sighup;
  int sigterm;
  int sigint;
};

void *ProbeMask(void *arg) {
  WorkerProbe *probe = static_cast<WorkerProbe *>(arg);
  sigset_t cur;
  sigemptyset(&cur);
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  probe->sighup  = sigismember(&cur, SIGHUP);
  probe->sigterm = sigismember(&cur, SIGTERM);
  probe->sigint  = sigismember(&cur, SIGINT);
  return nullptr;
}

class SignalInitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    /* Idempotent; blocks {SIGHUP,SIGTERM,SIGINT} on the calling thread. */
    InitSignals();
  }
};

/* ── §2 mask asserts ────────────────────────────────────────────────────── */

TEST_F(SignalInitTest, ControlSignalsAreBlocked) {
  sigset_t cur;
  sigemptyset(&cur);
  pthread_sigmask(SIG_SETMASK, NULL, &cur);

  EXPECT_EQ(sigismember(&cur, SIGHUP), 1);
  EXPECT_EQ(sigismember(&cur, SIGTERM), 1);
  EXPECT_EQ(sigismember(&cur, SIGINT), 1);
}

TEST_F(SignalInitTest, ControlSetGetterMatchesBlockedMask) {
  const sigset_t *set = UfsrvSignalControlSet();
  ASSERT_NE(set, nullptr);

  EXPECT_EQ(sigismember(set, SIGHUP), 1);
  EXPECT_EQ(sigismember(set, SIGTERM), 1);
  EXPECT_EQ(sigismember(set, SIGINT), 1);

  /* SIGPIPE is SIG_IGN'd, not blocked — must be absent from the signalfd set. */
  EXPECT_EQ(sigismember(set, SIGPIPE), 0);

  /* User-settable signals are async-handler stubs, not blocked — must be absent. */
  EXPECT_EQ(sigismember(set, SIGUSR1), 0);
  EXPECT_EQ(sigismember(set, SIGUSR2), 0);
}

TEST_F(SignalInitTest, SigpipeIsIgnored) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  ASSERT_EQ(sigaction(SIGPIPE, nullptr, &sa), 0);
  EXPECT_EQ(sa.sa_handler, SIG_IGN);
}

TEST_F(SignalInitTest, UserSettableSignalsHaveStubHandlers) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  ASSERT_EQ(sigaction(SIGUSR1, nullptr, &sa), 0);
  EXPECT_NE(sa.sa_handler, SIG_DFL);
  EXPECT_NE(sa.sa_handler, SIG_IGN);

  memset(&sa, 0, sizeof(sa));
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &sa), 0);
  EXPECT_NE(sa.sa_handler, SIG_DFL);
  EXPECT_NE(sa.sa_handler, SIG_IGN);
}

/* ── §3 worker inheritance (spawn-window exclusion) ─────────────────────── */

TEST_F(SignalInitTest, WorkerInheritsBlockedMask) {
  WorkerProbe probe = {0, 0, 0};
  pthread_t th;
  ASSERT_EQ(pthread_create(&th, nullptr, ProbeMask, &probe), 0);
  ASSERT_EQ(pthread_join(th, nullptr), 0);

  /* A worker created after InitSignals() must see the control signals blocked —
   * it therefore can never run a handler or take their default action. */
  EXPECT_EQ(probe.sighup, 1);
  EXPECT_EQ(probe.sigterm, 1);
  EXPECT_EQ(probe.sigint, 1);
}

/* ── §1 harness: signalfd wakes select, only main consumes ──────────────── */

TEST_F(SignalInitTest, SigtermWakesSignalfdSelect) {
  int sfd = signalfd(-1, UfsrvSignalControlSet(), SFD_NONBLOCK | SFD_CLOEXEC);
  ASSERT_GE(sfd, 0);
  DrainSignalfd(sfd);

  /* Process-directed, matching production kill(pid, SIGTERM). Blocked → pending. */
  ASSERT_EQ(kill(getpid(), SIGTERM), 0);

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(sfd, &rfds);
  struct timeval tv = {1, 0};
  int n = select(sfd + 1, &rfds, nullptr, nullptr, &tv);
  ASSERT_GT(n, 0) << "select() did not wake on signalfd (errno " << errno << ")";
  ASSERT_NE(FD_ISSET(sfd, &rfds), 0);

  struct signalfd_siginfo si;
  ASSERT_EQ(read(sfd, &si, sizeof(si)), static_cast<ssize_t>(sizeof(si)));
  EXPECT_EQ(si.ssi_signo, SIGTERM);

  close(sfd);
}

/* ── §4 flood: standard signals coalesce (bounded wakeups) ──────────────── */

TEST_F(SignalInitTest, StandardSignalsCoalesceUnderFlood) {
  int sfd = signalfd(-1, UfsrvSignalControlSet(), SFD_NONBLOCK | SFD_CLOEXEC);
  ASSERT_GE(sfd, 0);
  DrainSignalfd(sfd);

  /* 100× SIGHUP must not yield 100 events — standard signals coalesce. */
  for (int i = 0; i < 100; ++i) {
    ASSERT_EQ(kill(getpid(), SIGHUP), 0);
  }

  int count = 0;
  struct signalfd_siginfo si;
  while (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
    EXPECT_EQ(si.ssi_signo, SIGHUP);
    ++count;
  }
  EXPECT_EQ(count, 1) << "standard signals must coalesce to a single event";

  close(sfd);
}

/* ── §5 negative: the control-signal path has no handler that can fire ───── */

TEST_F(SignalInitTest, ControlSignalsHaveNoHandlerInstalled) {
  /* A blocked signal must not also have a handler: it is delivered to signalfd,
   * never to a handler. Assert the disposition is still the default. */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  ASSERT_EQ(sigaction(SIGTERM, nullptr, &sa), 0);
  EXPECT_EQ(sa.sa_handler, SIG_DFL);
}

}  // namespace
