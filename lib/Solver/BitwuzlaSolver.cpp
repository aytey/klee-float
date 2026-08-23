//===-- BitwuzlaSolver.cpp ------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "klee/Config/config.h"
#include "klee/Solver.h"

#ifdef ENABLE_BITWUZLA

#include "BitwuzlaBuilder.h"
#include "klee/Constraints.h"
#include "klee/SolverImpl.h"
#include "klee/SolverStats.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprUtil.h"
#include "klee/Internal/Support/ErrorHandling.h"

#include "llvm/Support/CommandLine.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
llvm::cl::opt<bool> BitwuzlaAbstraction(
    "bitwuzla-abstraction", llvm::cl::init(true),
    llvm::cl::desc("Use Bitwuzla's bit-vector abstraction (default=on, which "
                   "is Bitwuzla's own default)"));

llvm::cl::opt<std::string> BitwuzlaQueryDumpFile(
    "debug-bitwuzla-dump-queries", llvm::cl::init(""),
    llvm::cl::desc("Dump Bitwuzla's SMT-LIBv2 representation of each query to "
                   "the specified path"));
}

namespace klee {

class BitwuzlaSolverImpl : public SolverImpl {
private:
  BitwuzlaBuilder *builder;
  double timeout;
  SolverRunStatus runStatusCode;

  bool internalRunSolver(const Query &,
                         const std::vector<const Array *> *objects,
                         std::vector<std::vector<unsigned char> > *values,
                         bool &hasSolution);

public:
  BitwuzlaSolverImpl();
  ~BitwuzlaSolverImpl();

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

BitwuzlaSolverImpl::BitwuzlaSolverImpl()
    : builder(new BitwuzlaBuilder(/*autoClearConstructCache=*/false)),
      timeout(0.0), runStatusCode(SOLVER_RUN_STATUS_FAILURE) {
  assert(builder && "unable to create BitwuzlaBuilder");
}

BitwuzlaSolverImpl::~BitwuzlaSolverImpl() { delete builder; }

BitwuzlaSolver::BitwuzlaSolver() : Solver(new BitwuzlaSolverImpl()) {}

char *BitwuzlaSolver::getConstraintLog(const Query &query) {
  return impl->getConstraintLog(query);
}

void BitwuzlaSolver::setCoreSolverTimeout(double timeout) {
  impl->setCoreSolverTimeout(timeout);
}

char *BitwuzlaSolverImpl::getConstraintLog(const Query &query) {
  // Bitwuzla can print an SMT-LIB representation of a term, but assembling a
  // whole benchmark is not something KLEE needs from this backend; the Z3 and
  // STP backends remain available for query logging.
  klee_warning_once(
      0, "getConstraintLog() is not supported by the Bitwuzla backend");
  return strdup("");
}

bool BitwuzlaSolverImpl::computeTruth(const Query &query, bool &isValid) {
  bool hasSolution = false;
  if (!internalRunSolver(query, NULL, NULL, hasSolution))
    return false;
  isValid = !hasSolution;
  return true;
}

bool BitwuzlaSolverImpl::computeValue(const Query &query, ref<Expr> &result) {
  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char> > values;
  bool hasSolution;

  findSymbolicObjects(query.expr, objects);
  if (!computeInitialValues(query.withFalse(), objects, values, hasSolution))
    return false;
  assert(hasSolution && "state has invalid constraint set");

  Assignment a(objects, values);
  result = a.evaluate(query.expr);
  return true;
}

bool BitwuzlaSolverImpl::computeInitialValues(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<std::vector<unsigned char> > &values, bool &hasSolution) {
  return internalRunSolver(query, &objects, &values, hasSolution);
}

/// Parse one of Bitwuzla's raw bitvector value strings, which are binary and
/// most-significant-bit first, into a byte.
static unsigned char bvStringToByte(const char *s) {
  unsigned char v = 0;
  for (const char *p = s; *p; ++p) {
    v = (unsigned char)(v << 1);
    if (*p == '1')
      v |= 1;
  }
  return v;
}

bool BitwuzlaSolverImpl::internalRunSolver(
    const Query &query, const std::vector<const Array *> *objects,
    std::vector<std::vector<unsigned char> > *values, bool &hasSolution) {
  TimerStatIncrementer t(stats::queryTime);
  runStatusCode = SOLVER_RUN_STATUS_FAILURE;
  ++stats::queries;
  if (objects)
    ++stats::queryCounterexamples;

  BitwuzlaOptions *options = bitwuzla_options_new();
  // KLEE needs counter-examples, not just satisfiability.
  bitwuzla_set_option(options, BITWUZLA_OPT_PRODUCE_MODELS, 1);

  // Bitwuzla's own bit-vector abstraction, on by default in Bitwuzla and
  // therefore on in every measurement here so far. Turning it off is how to
  // ask what it is actually worth on this workload rather than on
  // Bitwuzla's own; the same question STP's --stp-bv-abstraction-width asks
  // from the other side.
  if (!BitwuzlaAbstraction)
    bitwuzla_set_option(options, BITWUZLA_OPT_ABSTRACTION, 0);
  if (timeout > 0.0) {
    // Per-query wall clock limit, in milliseconds.
    bitwuzla_set_option(options, BITWUZLA_OPT_TIME_LIMIT_PER,
                        (uint64_t)(timeout * 1000));
  }
  Bitwuzla *bzla = bitwuzla_new(builder->tm, options);

  for (ConstraintManager::const_iterator it = query.constraints.begin(),
                                         ie = query.constraints.end();
       it != ie; ++it)
    bitwuzla_assert(bzla, builder->construct(*it));

  // KLEE asks whether the constraints imply the query, so ask Bitwuzla for a
  // model of the constraints together with the query's negation.
  BitwuzlaTermHandle queryTerm = builder->construct(query.expr);
  bitwuzla_assert(bzla, BitwuzlaTermHandle(bitwuzla_mk_term1(
                            builder->tm, BITWUZLA_KIND_NOT, queryTerm)));

  // Side constraints have to come last: building everything above is what
  // generates them (see BitwuzlaBuilder::castToBitVector()).
  for (std::vector<BitwuzlaTermHandle>::iterator
           it = builder->sideConstraints.begin(),
           ie = builder->sideConstraints.end();
       it != ie; ++it)
    bitwuzla_assert(bzla, *it);

  if (!BitwuzlaQueryDumpFile.empty()) {
    // Unlike Z3's dump, this uses only standard SMT-LIB -- Bitwuzla has no
    // fp.to_ieee_bv -- so the result is portable to other solvers.
    FILE *f = fopen(BitwuzlaQueryDumpFile.c_str(), "a");
    if (f) {
      fprintf(f, "; start Bitwuzla query\n(set-logic QF_ABVFP)\n");
      bitwuzla_print_formula(bzla, "smt2", f, 10);
      fprintf(f, "(check-sat)\n(exit)\n; end Bitwuzla query\n\n");
      fclose(f);
    }
  }

  BitwuzlaResult result = bitwuzla_check_sat(bzla);

  bool success = true;
  switch (result) {
  case BITWUZLA_SAT: {
    hasSolution = true;
    runStatusCode = SOLVER_RUN_STATUS_SUCCESS_SOLVABLE;
    if (objects) {
      assert(values && "values cannot be nullptr");
      values->reserve(objects->size());
      for (std::vector<const Array *>::const_iterator it = objects->begin(),
                                                      ie = objects->end();
           it != ie; ++it) {
        const Array *array = *it;
        std::vector<unsigned char> data;
        data.reserve(array->size);
        for (unsigned offset = 0; offset < array->size; offset++) {
          BitwuzlaTermHandle initialRead =
              builder->getInitialRead(array, offset);
          ::BitwuzlaTerm value = bitwuzla_get_value(bzla, initialRead);
          data.push_back(bvStringToByte(bitwuzla_term_value_get_str(value)));
          bitwuzla_term_release(value);
        }
        values->push_back(data);
      }
    }
    break;
  }
  case BITWUZLA_UNSAT:
    hasSolution = false;
    runStatusCode = SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE;
    break;
  default:
    hasSolution = false;
    runStatusCode = timeout > 0.0 ? SOLVER_RUN_STATUS_TIMEOUT
                                  : SOLVER_RUN_STATUS_FAILURE;
    success = false;
    break;
  }

  bitwuzla_delete(bzla);
  bitwuzla_options_delete(options);

  // Terms are shared across a whole Query rather than a single construct()
  // call, so the cache is cleared here rather than by the builder.
  builder->clearConstructCache();
  // Re-asserting side constraints against a later query would be wrong, and
  // they are regenerated per query in any case.
  builder->clearSideConstraints();

  if (success) {
    if (hasSolution)
      ++stats::queriesInvalid;
    else
      ++stats::queriesValid;
  }
  return success;
}

SolverImpl::SolverRunStatus BitwuzlaSolverImpl::getOperationStatusCode() {
  return runStatusCode;
}
}
#endif // ENABLE_BITWUZLA
