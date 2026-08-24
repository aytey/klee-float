//===-- STPSolver.cpp -----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "klee/Config/config.h"
#ifdef ENABLE_STP
#include "STPBuilder.h"
#include "klee/Solver.h"
#include "klee/SolverImpl.h"
#include "klee/Constraints.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprUtil.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errno.h"
#include "llvm/Support/ErrorHandling.h"

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

namespace {

llvm::cl::opt<bool> DebugDumpSTPQueries(
    "debug-dump-stp-queries", llvm::cl::init(false),
    llvm::cl::desc("Dump every STP query to stderr (default=off)"));

// Where a query's time actually goes. KLEE's own QueryTime covers the whole
// call -- translating the KLEE expression into STP's C API, asserting it, the
// solve, and reading the counterexample back -- so a solver that looks slow
// there may not be solving slowly at all.
// STP has two ways of deciding a query: the batch pipeline, which simplifies
// the whole formula and then bit-blasts it, and a persistent incremental
// driver that keeps one solver across vc_push/vc_pop. KLEE pushes and pops
// per query, so STP's automatic engagement switches to the incremental driver
// from the third query onwards and stays there.
//
// That is the wrong trade for KLEE. Each of its queries is independent -- the
// constraint set is asserted and retracted whole -- so the incremental driver
// keeps state across queries that share nothing, and gives up the batch
// simplifications in return. On floating-point queries it costs about three
// times the solve time.
// Which query STP's incremental driver takes over on. STP's own default for
// an embedder is the third: the C API has no set-logic, so it cannot claim
// the longer threshold a sweep chose for pure bit-vector sessions, and every
// KLEE query from the third onwards has therefore been decided incrementally.
//
// Neither extreme is right. Over fp-bench the two modes come within 2% of
// each other in total and are wildly apart per benchmark -- batch is 6x
// better on sparse_matrices_klee_bug, incremental 3.4x better on
// sort_smallest_klee -- so what this really selects is which sessions get
// which, and the number of queries alone does not predict the winner.
//
// Eight, which under the adaptive policy below is not a decision but the
// point at which the measuring starts: eight batch queries are the baseline,
// and the driver is tried against it from the ninth. A fixed ordinal wants a
// much larger number (32 measured best) precisely because it cannot take the
// decision back.
llvm::cl::opt<int> STPIncrementalEngageAt(
    "stp-incremental-engage-at", llvm::cl::init(8),
    llvm::cl::desc("Query ordinal at which STP's incremental driver takes "
                   "over: 0 never (default), N on the Nth query, -1 to leave "
                   "STP's own policy alone"));

// Decide the incremental question by measurement instead of by ordinal.
//
// A fixed threshold has to guess, and the guess is wrong often: over fp-bench
// `libmatheval_sym_f` has 69 queries and wants the batch pipeline, while
// `sqr_longdouble-flow` has 6 and wants the driver. Picking correctly per
// session is worth about 13% over the best fixed ordinal, and no property of
// the session available up front predicts it.
//
// So: spend the first --stp-incremental-engage-at queries on the batch
// pipeline and remember what they cost, then engage the driver and keep
// comparing. If it turns out to be much worse, disengage for good; if it
// survives a full probe, keep it for good. The comparison is between means
// over different queries, which is crude -- but what it has to separate are
// three- to six-fold differences, and it only has to be right about the sign.
llvm::cl::opt<bool> STPAdaptIncremental(
    "stp-adapt-incremental", llvm::cl::init(true),
    llvm::cl::desc("Choose between STP's batch and incremental modes by "
                   "measuring both, rather than by a fixed query ordinal "
                   "(default=on)"));

llvm::cl::opt<double> STPAdaptRegret(
    "stp-adapt-regret", llvm::cl::init(2.0),
    llvm::cl::desc("How much slower per query the incremental driver may be "
                   "before it is abandoned (default=2.0)"));

// How hard STP works to shrink the CNF before handing it to the SAT solver.
//
// It is a trade, not a quality dial, and floating-point queries sit on both
// sides of it. A `sqrt` over a wide significand builds an enormous circuit
// that the SAT solver then disposes of immediately: on nan_double one query
// spent 983ms of its 1.28s in cut enumeration and none at all in search, and
// the whole benchmark halves at the low end. Queries whose search is the
// expensive part want the opposite -- vectors_klee is 6.9s at high effort
// against 15.7s at very low.
//
// -1 leaves STP's own default (medium) alone.
llvm::cl::opt<int> STPCNFEffort(
    "stp-cnf-effort", llvm::cl::init(-1),
    llvm::cl::desc("Effort STP spends minimising the CNF: 0 very low .. 4 "
                   "very high (default=-1, leave STP's own default alone)"));

llvm::cl::opt<bool> DebugSTPPhaseTiming(
    "debug-stp-phase-timing", llvm::cl::init(false),
    llvm::cl::desc("Report per-query build/assert and solve times for STP "
                   "(default=off)"));

llvm::cl::opt<bool> IgnoreSolverFailures(
    "ignore-solver-failures", llvm::cl::init(false),
    llvm::cl::desc("Ignore any solver failures (default=off)"));

// STP replaces a wide bit-vector operation by free result bits and pins them
// lazily, refining only where a candidate model contradicts the operands
// underneath. It is off in STP itself, and off here, because what it is worth
// depends entirely on the floor: too high and it engages on nothing, too low
// and it abstracts operations whose exact encoding was cheaper than the
// rounds spent avoiding it. On this benchmark set the significand product of
// a binary32 fp.mul is 24 to 33 bits wide, which is the range worth trying.
//
// Zero leaves it off, which is what STP does by default and what every
// measurement before this used.
llvm::cl::opt<unsigned> STPBVAbstractionWidth(
    "stp-bv-abstraction-width", llvm::cl::init(0),
    llvm::cl::desc("Operand width at or above which STP abstracts bit-vector "
                   "operations and refines them by CEGAR (default=0, off)"));

// How long the refinement enumerates operand values before giving up on an
// abstracted multiply and encoding it exactly. STP's own default is a flat
// count, whatever the operands' width; a nonzero divisor here makes it
// width/divisor instead. What one of those lemmas rules out is one pair out
// of 2^(2W), so a flat count means something quite different at 24 bits and
// at 64, and this benchmark set has both.
llvm::cl::opt<unsigned> STPBVAbstractionValueDivisor(
    "stp-bv-abstraction-value-divisor", llvm::cl::init(0),
    llvm::cl::desc("Scale STP's blocking-lemma allowance with the operand "
                   "width, as width/divisor (default=0, use STP's flat "
                   "allowance); only meaningful with "
                   "--stp-bv-abstraction-width"));
}

#define vc_bvBoolExtract IAMTHESPAWNOFSATAN

static unsigned char *shared_memory_ptr;
static int shared_memory_id = 0;
// Darwin by default has a very small limit on the maximum amount of shared
// memory, which will quickly be exhausted by KLEE running its tests in
// parallel. For now, we work around this by just requesting a smaller size --
// in practice users hitting this limit on counterexample sizes probably already
// are hitting more serious scalability issues.
#ifdef __APPLE__
static const unsigned shared_memory_size = 1 << 16;
#else
static const unsigned shared_memory_size = 1 << 20;
#endif

static long elapsedMillis(const struct timeval &a, const struct timeval &b) {
  return (b.tv_sec - a.tv_sec) * 1000L + (b.tv_usec - a.tv_usec) / 1000L;
}

static void stp_error_handler(const char *err_msg) {
  fprintf(stderr, "error: STP Error: %s\n", err_msg);
  abort();
}

namespace klee {

class STPSolverImpl : public SolverImpl {
private:
  VC vc;
  STPBuilder *builder;
  double timeout;
  bool useForkedSTP;
  SolverRunStatus runStatusCode;

  // Adaptive mode selection; see STPAdaptIncremental.
  size_t queriesRun;
  double batchSeconds, incrementalSeconds;
  size_t batchQueries, incrementalQueries;
  bool incrementalEngaged;  // what STP was last told
  bool modeSettled;         // stop probing: the decision is made

  // Whether this query should be handed to the incremental driver, and
  // telling STP so.
  void selectMode();
  // Fold one query's cost into whichever mode ran it, and revisit.
  void recordQueryCost(double seconds);

public:
  STPSolverImpl(bool _useForkedSTP, bool _optimizeDivides = true);
  ~STPSolverImpl();

  char *getConstraintLog(const Query &);
  void setCoreSolverTimeout(double _timeout) { timeout = _timeout; }

  bool computeTruth(const Query &, bool &isValid);
  bool computeValue(const Query &, ref<Expr> &result);
  bool computeInitialValues(const Query &,
                            const std::vector<const Array *> &objects,
                            std::vector<std::vector<unsigned char> > &values,
                            bool &hasSolution);
  SolverRunStatus getOperationStatusCode();
};

STPSolverImpl::STPSolverImpl(bool _useForkedSTP, bool _optimizeDivides)
    : vc(vc_createValidityChecker()),
      builder(new STPBuilder(vc, _optimizeDivides)), timeout(0.0),
      useForkedSTP(_useForkedSTP), runStatusCode(SOLVER_RUN_STATUS_FAILURE),
      queriesRun(0), batchSeconds(0.0), incrementalSeconds(0.0),
      batchQueries(0), incrementalQueries(0), incrementalEngaged(false),
      modeSettled(false) {
  assert(vc && "unable to create validity checker");
  assert(builder && "unable to create STPBuilder");

  // In newer versions of STP, a memory management mechanism has been
  // introduced that automatically invalidates certain C interface
  // pointers at vc_Destroy time.  This caused double-free errors
  // due to the ExprHandle destructor also attempting to invalidate
  // the pointers using vc_DeleteExpr.  By setting EXPRDELETE to 0
  // we restore the old behaviour.
  vc_setInterfaceFlags(vc, EXPRDELETE, 0);

  // See STPIncrementalEngageAt above; negative leaves STP's own policy
  // alone. Under the adaptive policy selectMode() drives this per query
  // instead, starting from the batch pipeline.
  if (STPIncrementalEngageAt >= 0)
    vc_setInterfaceFlags(vc, INCREMENTAL_AUTO_ENGAGE_AT,
                         STPAdaptIncremental
                             ? 0
                             : STPIncrementalEngageAt.getValue());

  if (STPBVAbstractionWidth > 0) {
    vc_setInterfaceFlags(vc, BV_ABSTRACTION_WIDTH,
                         (int)STPBVAbstractionWidth.getValue());
    vc_setInterfaceFlags(vc, BV_EQ_ABSTRACTION, 1);
    vc_setInterfaceFlags(vc, BV_TERM_ABSTRACTION, 1);
    if (STPBVAbstractionValueDivisor > 0)
      vc_setInterfaceFlags(vc, BV_TERM_ABSTRACTION_VALUE_DIVISOR,
                           (int)STPBVAbstractionValueDivisor.getValue());
  }

  if (STPCNFEffort >= 0)
    vc_setInterfaceFlags(vc, CNF_GENERATION_EFFORT, STPCNFEffort.getValue());

  make_division_total(vc);

  vc_registerErrorHandler(::stp_error_handler);

  if (useForkedSTP) {
    assert(shared_memory_id == 0 && "shared memory id already allocated");
    shared_memory_id =
        shmget(IPC_PRIVATE, shared_memory_size, IPC_CREAT | 0700);
    if (shared_memory_id < 0)
      llvm::report_fatal_error("unable to allocate shared memory region");
    shared_memory_ptr = (unsigned char *)shmat(shared_memory_id, NULL, 0);
    if (shared_memory_ptr == (void *)-1)
      llvm::report_fatal_error("unable to attach shared memory region");
    shmctl(shared_memory_id, IPC_RMID, NULL);
  }
}

STPSolverImpl::~STPSolverImpl() {
  // Detach the memory region.
  shmdt(shared_memory_ptr);
  shared_memory_ptr = 0;
  shared_memory_id = 0;

  delete builder;

  vc_Destroy(vc);
}

/***/

char *STPSolverImpl::getConstraintLog(const Query &query) {
  vc_push(vc);
  for (std::vector<ref<Expr> >::const_iterator it = query.constraints.begin(),
                                               ie = query.constraints.end();
       it != ie; ++it)
    vc_assertFormula(vc, builder->construct(*it));
  assert(query.expr == ConstantExpr::alloc(0, Expr::Bool) &&
         "Unexpected expression in query!");

  for (std::vector<ExprHandle>::iterator
           it = builder->sideConstraints.begin(),
           ie = builder->sideConstraints.end();
       it != ie; ++it)
    vc_assertFormula(vc, *it);

  char *buffer;
  unsigned long length;
  vc_printQueryStateToBuffer(vc, builder->getFalse(), &buffer, &length, false);
  vc_pop(vc);
  builder->clearSideConstraints();

  return buffer;
}

bool STPSolverImpl::computeTruth(const Query &query, bool &isValid) {
  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char> > values;
  bool hasSolution;

  if (!computeInitialValues(query, objects, values, hasSolution))
    return false;

  isValid = !hasSolution;
  return true;
}

bool STPSolverImpl::computeValue(const Query &query, ref<Expr> &result) {
  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char> > values;
  bool hasSolution;

  // Find the object used in the expression, and compute an assignment
  // for them.
  findSymbolicObjects(query.expr, objects);
  if (!computeInitialValues(query.withFalse(), objects, values, hasSolution))
    return false;
  assert(hasSolution && "state has invalid constraint set");

  // Evaluate the expression with the computed assignment.
  Assignment a(objects, values);
  result = a.evaluate(query.expr);

  return true;
}

static SolverImpl::SolverRunStatus
runAndGetCex(::VC vc, STPBuilder *builder, ::VCExpr q,
             const std::vector<const Array *> &objects,
             std::vector<std::vector<unsigned char> > &values,
             bool &hasSolution) {
  // XXX I want to be able to timeout here, safely
  struct timeval qStart, qEnd;
  gettimeofday(&qStart, NULL);
  hasSolution = !vc_query(vc, q);
  gettimeofday(&qEnd, NULL);
  if (DebugSTPPhaseTiming)
    klee_warning("STP query: vc_query %ldms", elapsedMillis(qStart, qEnd));

  if (hasSolution) {
    values.reserve(objects.size());
    for (std::vector<const Array *>::const_iterator it = objects.begin(),
                                                    ie = objects.end();
         it != ie; ++it) {
      const Array *array = *it;
      std::vector<unsigned char> data;

      data.reserve(array->size);
      for (unsigned offset = 0; offset < array->size; offset++) {
        ExprHandle counter =
            vc_getCounterExample(vc, builder->getInitialRead(array, offset));
        unsigned char val = getBVUnsigned(counter);
        data.push_back(val);
      }

      values.push_back(data);
    }
  }

  if (true == hasSolution) {
    return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_SOLVABLE;
  } else {
    return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE;
  }
}

static void stpTimeoutHandler(int x) { _exit(52); }

static SolverImpl::SolverRunStatus
runAndGetCexForked(::VC vc, STPBuilder *builder, ::VCExpr q,
                   const std::vector<const Array *> &objects,
                   std::vector<std::vector<unsigned char> > &values,
                   bool &hasSolution, double timeout) {
  unsigned char *pos = shared_memory_ptr;
  unsigned sum = 0;
  for (std::vector<const Array *>::const_iterator it = objects.begin(),
                                                  ie = objects.end();
       it != ie; ++it)
    sum += (*it)->size;
  if (sum >= shared_memory_size)
    llvm::report_fatal_error("not enough shared memory for counterexample");

  fflush(stdout);
  fflush(stderr);
  int pid = fork();
  if (pid == -1) {
    klee_warning("fork failed (for STP) - %s", llvm::sys::StrError(errno).c_str());
    if (!IgnoreSolverFailures)
      exit(1);
    return SolverImpl::SOLVER_RUN_STATUS_FORK_FAILED;
  }

  if (pid == 0) {
    if (timeout) {
      ::alarm(0); /* Turn off alarm so we can safely set signal handler */
      ::signal(SIGALRM, stpTimeoutHandler);
      ::alarm(std::max(1, (int)timeout));
    }
    unsigned res = vc_query(vc, q);
    if (!res) {
      for (std::vector<const Array *>::const_iterator it = objects.begin(),
                                                      ie = objects.end();
           it != ie; ++it) {
        const Array *array = *it;
        for (unsigned offset = 0; offset < array->size; offset++) {
          ExprHandle counter =
              vc_getCounterExample(vc, builder->getInitialRead(array, offset));
          *pos++ = getBVUnsigned(counter);
        }
      }
    }
    _exit(res);
  } else {
    int status;
    pid_t res;

    do {
      res = waitpid(pid, &status, 0);
    } while (res < 0 && errno == EINTR);

    if (res < 0) {
      klee_warning("waitpid() for STP failed");
      if (!IgnoreSolverFailures)
        exit(1);
      return SolverImpl::SOLVER_RUN_STATUS_WAITPID_FAILED;
    }

    // From timed_run.py: It appears that linux at least will on
    // "occasion" return a status when the process was terminated by a
    // signal, so test signal first.
    if (WIFSIGNALED(status) || !WIFEXITED(status)) {
      klee_warning("STP did not return successfully.  Most likely you forgot "
                   "to run 'ulimit -s unlimited'");
      if (!IgnoreSolverFailures) {
        exit(1);
      }
      return SolverImpl::SOLVER_RUN_STATUS_INTERRUPTED;
    }

    int exitcode = WEXITSTATUS(status);
    if (exitcode == 0) {
      hasSolution = true;
    } else if (exitcode == 1) {
      hasSolution = false;
    } else if (exitcode == 52) {
      klee_warning("STP timed out");
      // mark that a timeout occurred
      return SolverImpl::SOLVER_RUN_STATUS_TIMEOUT;
    } else {
      klee_warning("STP did not return a recognized code");
      if (!IgnoreSolverFailures)
        exit(1);
      return SolverImpl::SOLVER_RUN_STATUS_UNEXPECTED_EXIT_CODE;
    }

    if (hasSolution) {
      values = std::vector<std::vector<unsigned char> >(objects.size());
      unsigned i = 0;
      for (std::vector<const Array *>::const_iterator it = objects.begin(),
                                                      ie = objects.end();
           it != ie; ++it) {
        const Array *array = *it;
        std::vector<unsigned char> &data = values[i++];
        data.insert(data.begin(), pos, pos + array->size);
        pos += array->size;
      }
    }

    if (true == hasSolution) {
      return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_SOLVABLE;
    } else {
      return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE;
    }
  }
}
// Which mode this query runs in.
//
// The batch phase comes first because it is the safe one: it is the classic
// pipeline, it is what a session of two or three queries should be getting
// anyway, and its cost is the baseline everything else is compared against.
void STPSolverImpl::selectMode() {
  if (!STPAdaptIncremental) {
    // Fixed policy: whatever the ordinal says, set once and left alone.
    return;
  }

  bool wantIncremental;
  if (modeSettled) {
    wantIncremental = incrementalEngaged;
  } else if (STPIncrementalEngageAt <= 0) {
    wantIncremental = false;  // never engage, so nothing to measure
  } else {
    wantIncremental = queriesRun >= (size_t)STPIncrementalEngageAt;
  }

  if (wantIncremental != incrementalEngaged || queriesRun == 0) {
    // 1 engages from the next query, 0 prevents automatic engagement. The
    // flag is read per query, so this is a switch and not a one-off.
    vc_setInterfaceFlags(vc, INCREMENTAL_AUTO_ENGAGE_AT,
                         wantIncremental ? 1 : 0);
    incrementalEngaged = wantIncremental;
  }
}

// Fold this query's cost into whichever mode ran it, and decide whether to
// keep going.
//
// Two ways to settle. The driver is abandoned as soon as it is clearly
// losing -- four queries is enough to see a threefold difference and few
// enough not to have paid much for it -- and it is adopted for good once it
// has matched the batch phase over a probe as long as the batch phase was.
// A session that ends before either is a session where the ordinal decided,
// which is the behaviour this replaced.
void STPSolverImpl::recordQueryCost(double seconds) {
  queriesRun++;
  if (!STPAdaptIncremental || modeSettled)
    return;

  if (incrementalEngaged) {
    incrementalSeconds += seconds;
    incrementalQueries++;
  } else {
    batchSeconds += seconds;
    batchQueries++;
  }

  // A baseline of one or two queries is not a baseline: KLEE's opening
  // queries are often trivial, and a mean taken over them would condemn the
  // driver for being slower than nothing.
  if (batchQueries < 4 || incrementalQueries == 0)
    return;

  const double batchMean = batchSeconds / (double)batchQueries;
  const double incMean = incrementalSeconds / (double)incrementalQueries;

  // Nothing to tell apart while both are noise. Comparing means of
  // sub-millisecond queries would settle the mode on scheduling jitter.
  if (batchMean < 1e-3 && incMean < 1e-3)
    return;

  if (incrementalQueries >= 4 && incMean > STPAdaptRegret * batchMean) {
    modeSettled = true;
    incrementalEngaged = false;
    vc_setInterfaceFlags(vc, INCREMENTAL_AUTO_ENGAGE_AT, 0);
    if (DebugSTPPhaseTiming)
      klee_warning("STP: abandoning the incremental driver after %zu queries "
                   "(%.1fms/query against %.1fms batch)",
                   incrementalQueries, incMean * 1000.0, batchMean * 1000.0);
    return;
  }

  // There is deliberately no matching "keep it for good". Settling on the
  // driver after a short probe locks in whatever the first few queries
  // happened to cost -- with a four-query baseline that fired on the fourth
  // engaged query and kept the driver on a session that ran five times
  // slower for the next nine hundred. Staying unsettled costs nothing: the
  // means keep accumulating, and the test above can still fire much later.
}

bool STPSolverImpl::computeInitialValues(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<std::vector<unsigned char> > &values, bool &hasSolution) {
  runStatusCode = SOLVER_RUN_STATUS_FAILURE;

  TimerStatIncrementer t(stats::queryTime);

  vc_push(vc);

  for (ConstraintManager::const_iterator it = query.constraints.begin(),
                                         ie = query.constraints.end();
       it != ie; ++it)
    vc_assertFormula(vc, builder->construct(*it));

  ++stats::queries;
  ++stats::queryCounterexamples;

  struct timeval phase0, phase1, phase2;
  selectMode();
  gettimeofday(&phase0, NULL);
  ExprHandle stp_e = builder->construct(query.expr);

  // Assert any side constraints generated while building the query (see
  // STPBuilder::castToFloat()). This has to come last, once everything has
  // been traversed, so that we have all of them.
  for (std::vector<ExprHandle>::iterator
           it = builder->sideConstraints.begin(),
           ie = builder->sideConstraints.end();
       it != ie; ++it)
    vc_assertFormula(vc, *it);

  if (DebugDumpSTPQueries) {
    // SMT-LIB2, not the CVC presentation language. vc_printQueryStateToBuffer
    // goes through PL_Print, which predates the floating-point theory and
    // refuses it with a fatal error -- so this option aborted KLEE on exactly
    // the queries anyone dumping floating-point queries wants to see. What
    // comes back is a self-contained script: the asserted constraints and the
    // negated query, ready to re-run through `stp --SMTLIB2`.
    //
    // The string is deliberately not freed. Its contract says the caller
    // owns it, but STP links a vendored mimalloc that replaces malloc and
    // free inside libstp, so the buffer did not come from the allocator this
    // translation unit's free() belongs to -- handing it back aborted the
    // process on the first query dumped. One leaked string per query, in a
    // debugging option that prints every query to stderr, is the cheaper
    // side of that trade.
    if (char *smt = vc_printSMTLIB2(vc, vc_notExpr(vc, stp_e)))
      klee_warning("STP query:\n%s\n", smt);
  }

  gettimeofday(&phase1, NULL);

  bool success;
  if (useForkedSTP) {
    runStatusCode = runAndGetCexForked(vc, builder, stp_e, objects, values,
                                       hasSolution, timeout);
    success = ((SOLVER_RUN_STATUS_SUCCESS_SOLVABLE == runStatusCode) ||
               (SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE == runStatusCode));
  } else {
    runStatusCode =
        runAndGetCex(vc, builder, stp_e, objects, values, hasSolution);
    success = true;
  }

  if (success) {
    if (hasSolution)
      ++stats::queriesInvalid;
    else
      ++stats::queriesValid;
  }

  gettimeofday(&phase2, NULL);
  if (DebugSTPPhaseTiming)
    klee_warning("STP query: build+assert %ldms, solve+cex %ldms%s",
                 elapsedMillis(phase0, phase1), elapsedMillis(phase1, phase2),
                 incrementalEngaged ? " [incremental]" : " [batch]");
  recordQueryCost(elapsedMillis(phase1, phase2) / 1000.0);

  vc_pop(vc);

  // Any generated side constraints could break subsequent queries if we were
  // to assert them again, and they are re-generated per query anyway.
  builder->clearSideConstraints();

  return success;
}

SolverImpl::SolverRunStatus STPSolverImpl::getOperationStatusCode() {
  return runStatusCode;
}

STPSolver::STPSolver(bool useForkedSTP, bool optimizeDivides)
    : Solver(new STPSolverImpl(useForkedSTP, optimizeDivides)) {}

char *STPSolver::getConstraintLog(const Query &query) {
  return impl->getConstraintLog(query);
}

void STPSolver::setCoreSolverTimeout(double timeout) {
  impl->setCoreSolverTimeout(timeout);
}
}
#endif // ENABLE_STP
