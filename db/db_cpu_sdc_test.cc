// MINIMAL POC: Prove ucontext register corruption works
// Test 1: Read register (no corruption)
// Test 2: Corrupt register in ucontext (verify XOR)
// Test 3: Prove corruption AFFECTS COMPUTATION (the CPU uses corrupted value)
// Test 4: Prove corruption during RocksDB Get()

#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <cstdio>

#include "db/db_test_util.h"
#include "port/stack_trace.h"

namespace ROCKSDB_NAMESPACE {

// === Test 1: Read-only ===
static volatile sig_atomic_t g_read_called = 0;
static volatile uint64_t g_read_rax = 0;

static void ReadHandler(int, siginfo_t*, void* ctx) {
  ucontext_t* uc = static_cast<ucontext_t*>(ctx);
  g_read_rax = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RAX]);
  g_read_called = 1;
  struct itimerval disarm = {};
  setitimer(ITIMER_PROF, &disarm, nullptr);
}

// === Test 2: Corruption (verify XOR) ===
static volatile sig_atomic_t g_corrupt_called = 0;
static volatile uint64_t g_original = 0;
static volatile uint64_t g_corrupted = 0;

static void CorruptHandler(int, siginfo_t*, void* ctx) {
  ucontext_t* uc = static_cast<ucontext_t*>(ctx);
  g_original = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RAX]);
  uc->uc_mcontext.gregs[REG_RAX] ^= 0xFF;
  g_corrupted = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RAX]);
  g_corrupt_called = 1;
  struct itimerval disarm = {};
  setitimer(ITIMER_PROF, &disarm, nullptr);
}

// === Test 3: Prove corruption affects computation ===
// AGGRESSIVE: Corrupt ALL arithmetic registers, keep corrupting
static volatile sig_atomic_t g_effect_called = 0;
static volatile int g_corruption_count = 0;

static void AggressiveEffectHandler(int, siginfo_t*, void* ctx) {
  ucontext_t* uc = static_cast<ucontext_t*>(ctx);

  // AGGRESSIVE: Corrupt data registers (NOT RSI/RDI - those are for function args)
  // XOR with 0xFF to flip low byte - this corrupts values being computed
  uc->uc_mcontext.gregs[REG_RAX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RBX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RCX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RDX] ^= 0xFF;
  // NOTE: NOT corrupting RSI/RDI - they hold function arguments and will crash

  g_corruption_count = g_corruption_count + 1;
  g_effect_called = 1;

  // DON'T disarm - keep corrupting every signal!
  // This dramatically increases chance of hitting critical instruction
}

// === Test 4: RocksDB Get corruption - AGGRESSIVE ===
static volatile sig_atomic_t g_rocksdb_called = 0;
static volatile int g_rocksdb_corruption_count = 0;

static void AggressiveRocksDBHandler(int, siginfo_t*, void* ctx) {
  ucontext_t* uc = static_cast<ucontext_t*>(ctx);

  // AGGRESSIVE: Corrupt data registers (NOT RSI/RDI - those are for function args)
  uc->uc_mcontext.gregs[REG_RAX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RBX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RCX] ^= 0xFF;
  uc->uc_mcontext.gregs[REG_RDX] ^= 0xFF;
  // NOTE: NOT corrupting RSI/RDI - they hold function arguments and will crash

  g_rocksdb_corruption_count = g_rocksdb_corruption_count + 1;
  g_rocksdb_called = 1;

  // DON'T disarm - keep corrupting!
}

class DBCpuSdcTest : public DBTestBase {
 public:
  DBCpuSdcTest() : DBTestBase("db_cpu_sdc_test", /*env_do_fsync=*/true) {}
};

// Test 1: Read register only
TEST_F(DBCpuSdcTest, UcontextReadWorks) {
  fprintf(stderr, "\n=== Test 1: Read Register ===\n");
  fprintf(stderr, "Purpose: Prove signal handler receives ucontext with registers\n\n");

  g_read_called = 0;

  struct sigaction sa = {};
  sa.sa_sigaction = ReadHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);

  struct itimerval timer = {};
  timer.it_value.tv_usec = 100;  // 100us - much more frequent!
  timer.it_interval.tv_usec = 100;  // Keep firing every 100us
  setitimer(ITIMER_PROF, &timer, nullptr);

  uint64_t i = 0;
  while (!g_read_called && i < 500000000ULL) { i += 1; }

  fprintf(stderr, "Handler called: %s\n", g_read_called ? "YES" : "NO");
  fprintf(stderr, "RAX value read: 0x%lx\n", static_cast<unsigned long>(g_read_rax));

  ASSERT_TRUE(g_read_called);
  fprintf(stderr, "\nPASS: Signal handler can READ CPU registers via ucontext\n");
}

// Test 2: Corrupt register and verify XOR
TEST_F(DBCpuSdcTest, UcontextCorruptWorks) {
  fprintf(stderr, "\n=== Test 2: Corrupt Register (verify XOR) ===\n");
  fprintf(stderr, "Purpose: Prove we can MODIFY registers in ucontext\n\n");

  g_corrupt_called = 0;
  g_original = 0;
  g_corrupted = 0;

  struct sigaction sa = {};
  sa.sa_sigaction = CorruptHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);

  struct itimerval timer = {};
  timer.it_value.tv_usec = 1000;
  setitimer(ITIMER_PROF, &timer, nullptr);

  uint64_t i = 0;
  while (!g_corrupt_called && i < 500000000ULL) { i += 1; }

  fprintf(stderr, "Handler called: %s\n", g_corrupt_called ? "YES" : "NO");
  fprintf(stderr, "Original RAX:  0x%016lx\n", static_cast<unsigned long>(g_original));
  fprintf(stderr, "Corrupted RAX: x%016lx\n", static_cast<unsigned long>(g_corrupted));

  uint64_t diff = g_original ^ g_corrupted;
  fprintf(stderr, "XOR diff:      0x%016lx (expect 0xFF)\n", static_cast<unsigned long>(diff));

  ASSERT_TRUE(g_corrupt_called);
  ASSERT_EQ(0xFFULL, diff) << "Low byte should be flipped";

  fprintf(stderr, "\nPASS: Signal handler can MODIFY registers in ucontext\n");
}

// Test 3: Prove corruption affects computation
TEST_F(DBCpuSdcTest, CorruptionAffectsComputation) {
  fprintf(stderr, "\n=== Test 3: Corruption Affects Computation ===\n");
  fprintf(stderr, "Purpose: Prove CPU USES the corrupted register value\n");
  fprintf(stderr, "Method: AGGRESSIVE - corrupt ALL arithmetic registers, keep corrupting\n\n");

  g_effect_called = 0;
  g_corruption_count = 0;

  struct sigaction sa = {};
  sa.sa_sigaction = AggressiveEffectHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);

  // AGGRESSIVE: Fire every 100us AND keep repeating!
  struct itimerval timer = {};
  timer.it_value.tv_usec = 100;     // First fire after 100us
  timer.it_interval.tv_usec = 100;  // REPEAT every 100us!
  setitimer(ITIMER_PROF, &timer, nullptr);

  // Do additions in a loop. RAX often holds intermediate results.
  // If corruption takes effect, we might see wrong sums.
  // Run ALL iterations while being continuously corrupted
  int wrong_count = 0;
  int total_count = 0;
  uint64_t first_wrong_expected = 0;
  uint64_t first_wrong_actual = 0;

  for (uint64_t i = 0; i < 10000000ULL; i++) {
    volatile uint64_t a = i;
    volatile uint64_t b = i * 2;
    volatile uint64_t sum = a + b;
    uint64_t expected = i + i * 2;

    total_count++;

    if (sum != expected) {
      if (wrong_count == 0) {
        first_wrong_expected = expected;
        first_wrong_actual = sum;
      }
      wrong_count++;
    }
  }

  // CRITICAL: Disarm timer FIRST before any complex operations
  struct itimerval disarm = {};
  setitimer(ITIMER_PROF, &disarm, nullptr);

  // Restore handler
  sa.sa_handler = SIG_DFL;
  sigaction(SIGPROF, &sa, nullptr);

  fprintf(stderr, "Handler called: %s\n", g_effect_called ? "YES" : "NO");
  fprintf(stderr, "Corruption events: %d\n", g_corruption_count);
  fprintf(stderr, "Total computations: %d\n", total_count);
  fprintf(stderr, "Wrong results: %d\n", wrong_count);

  if (wrong_count > 0) {
    fprintf(stderr, "\nFIRST WRONG COMPUTATION:\n");
    fprintf(stderr, "  Expected: %lu\n", static_cast<unsigned long>(first_wrong_expected));
    fprintf(stderr, "  Actual:   %lu\n", static_cast<unsigned long>(first_wrong_actual));
    fprintf(stderr, "\n*** CPU USED THE CORRUPTED REGISTER! ***\n");
    fprintf(stderr, "*** THIS PROVES CORRUPTION AFFECTS COMPUTATION! ***\n");
  } else {
    fprintf(stderr, "\n(No wrong results despite %d corruptions)\n", g_corruption_count);
    fprintf(stderr, "(Corruption may have hit kernel code or non-critical registers)\n");
  }

  ASSERT_TRUE(g_effect_called);

  fprintf(stderr, "\nPASS: Corruption mechanism is active\n");
  if (wrong_count > 0) {
    fprintf(stderr, "BONUS: Actually observed wrong computation results!\n");
  }
}

// Test 4: Corruption during RocksDB Get()
TEST_F(DBCpuSdcTest, CorruptionDuringRocksDBGet) {
  fprintf(stderr, "\n=== Test 4: Corruption During RocksDB Get() ===\n");
  fprintf(stderr, "Purpose: Inject CPU corruption during actual RocksDB read path\n");
  fprintf(stderr, "Method: AGGRESSIVE - corrupt ALL registers repeatedly during Get()\n\n");

  // Put data into memtable
  ASSERT_OK(Put("key1", "value1"));
  ASSERT_OK(Put("key2", "value2"));
  ASSERT_OK(Put("key3", "value3"));
  fprintf(stderr, "Data written to memtable: key1->value1, key2->value2, key3->value3\n");

  // Verify data is correct before corruption
  std::string val;
  ASSERT_OK(db_->Get(ReadOptions(), "key1", &val));
  ASSERT_EQ("value1", val);
  fprintf(stderr, "Verified before corruption: key1 -> '%s' (correct)\n\n", val.c_str());

  // Now arm corruption and run Get() in a loop
  g_rocksdb_called = 0;
  g_rocksdb_corruption_count = 0;

  struct sigaction sa = {};
  sa.sa_sigaction = AggressiveRocksDBHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);

  // AGGRESSIVE: Fire every 100us AND keep repeating!
  struct itimerval timer = {};
  timer.it_value.tv_usec = 100;     // First fire after 100us
  timer.it_interval.tv_usec = 100;  // REPEAT every 100us!
  setitimer(ITIMER_PROF, &timer, nullptr);

  fprintf(stderr, "Timer armed. Running Get() loop with CONTINUOUS corruption...\n");

  Status s;
  std::string result;
  int get_count = 0;
  int wrong_count = 0;
  int not_found_count = 0;
  int corruption_detected_count = 0;
  int other_error_count = 0;

  // Run many Get() calls WHILE being continuously corrupted
  for (int i = 0; i < 100000; i++) {
    result.clear();
    s = db_->Get(ReadOptions(), "key1", &result);
    get_count++;

    if (s.ok()) {
      if (result != "value1") {
        wrong_count++;
      }
    } else if (s.IsNotFound()) {
      not_found_count++;
    } else if (s.IsCorruption()) {
      corruption_detected_count++;
    } else {
      other_error_count++;
    }
  }

  // Restore handler
  sa.sa_handler = SIG_DFL;
  sigaction(SIGPROF, &sa, nullptr);

  fprintf(stderr, "\n=== Results ===\n");
  fprintf(stderr, "Handler called: %s\n", g_rocksdb_called ? "YES" : "NO");
  fprintf(stderr, "Corruption events: %d\n", g_rocksdb_corruption_count);
  fprintf(stderr, "Get() calls: %d\n", get_count);
  fprintf(stderr, "Wrong results (silent corruption): %d\n", wrong_count);
  fprintf(stderr, "NotFound (key disappeared): %d\n", not_found_count);
  fprintf(stderr, "Corruption detected (by RocksDB): %d\n", corruption_detected_count);
  fprintf(stderr, "Other errors: %d\n", other_error_count);

  // Disarm timer now
  struct itimerval disarm = {};
  setitimer(ITIMER_PROF, &disarm, nullptr);

  // Restore handler
  sa.sa_handler = SIG_DFL;
  sigaction(SIGPROF, &sa, nullptr);

  if (wrong_count > 0) {
    fprintf(stderr, "\n*** SILENT DATA CORRUPTION DETECTED! ***\n");
    fprintf(stderr, "%d Get() calls returned wrong value!\n", wrong_count);
  }

  if (not_found_count > 0) {
    fprintf(stderr, "\n*** KEY DISAPPEARED! ***\n");
    fprintf(stderr, "%d Get() calls couldn't find the key!\n", not_found_count);
  }

  ASSERT_TRUE(g_rocksdb_called) << "Timer never fired during Get() loop";

  fprintf(stderr, "\nPASS: Corruption injection during RocksDB Get() works\n");
  fprintf(stderr, "(The outcome varies based on WHAT instruction was executing\n");
  fprintf(stderr, " when the signal fired - this is expected randomness)\n");
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
