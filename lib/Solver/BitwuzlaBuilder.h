//===-- BitwuzlaBuilder.h ---------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __UTIL_BITWUZLABUILDER_H__
#define __UTIL_BITWUZLABUILDER_H__

#include "klee/util/ExprHashMap.h"
#include "klee/util/ArrayExprHash.h"
#include "klee/Config/config.h"

#include <map>
#include <vector>

#include <bitwuzla/c/bitwuzla.h>

namespace klee {

/// Every Bitwuzla API call that returns a term hands back a reference the
/// caller owns, so a builder that never releases them grows the term manager
/// without bound over a run. This holds one such reference.
class BitwuzlaTermHandle {
  ::BitwuzlaTerm term;

public:
  BitwuzlaTermHandle() : term(NULL) {}
  /// Takes ownership of the reference `t` carries.
  BitwuzlaTermHandle(::BitwuzlaTerm t) : term(t) {}
  BitwuzlaTermHandle(const BitwuzlaTermHandle &b) : term(b.term) {
    if (term)
      ::bitwuzla_term_copy(term);
  }
  ~BitwuzlaTermHandle() {
    if (term)
      ::bitwuzla_term_release(term);
  }
  BitwuzlaTermHandle &operator=(const BitwuzlaTermHandle &b) {
    if (b.term)
      ::bitwuzla_term_copy(b.term);
    if (term)
      ::bitwuzla_term_release(term);
    term = b.term;
    return *this;
  }
  bool isNull() const { return term == NULL; }
  operator ::BitwuzlaTerm() const { return term; }
};

class BitwuzlaArrayExprHash : public ArrayExprHash<BitwuzlaTermHandle> {
  friend class BitwuzlaBuilder;

public:
  BitwuzlaArrayExprHash() {}
  virtual ~BitwuzlaArrayExprHash();
  void clear();
  void clearUpdates();
};

class BitwuzlaBuilder {
  friend class BitwuzlaSolverImpl;

  ExprHashMap<std::pair<BitwuzlaTermHandle, unsigned> > constructed;
  BitwuzlaArrayExprHash _arr_hash;

  /// Bitwuzla has no float-to-bits operation (`fp.to_ieee_bv` is a Z3
  /// extension rather than SMT-LIB), so castToBitVector() introduces a
  /// bitvector variable constrained to reinterpret as the float. Cache them,
  /// so that casting the same float twice yields the same variable rather than
  /// two that a model could disagree on.
  std::map< ::BitwuzlaTerm, BitwuzlaTermHandle> floatToBitVectorVars;

  /// Constraints generated as a side effect of translating, rather than by any
  /// one Expr: the bits/float correspondence above, and the x87 fp80 explicit
  /// significand bit. Clients must assert these alongside the query, once it
  /// has been fully constructed, and clear them afterwards.
  std::vector<BitwuzlaTermHandle> sideConstraints;

  unsigned freshVariableCounter;

private:
  BitwuzlaTermHandle bvOne(unsigned width);
  BitwuzlaTermHandle bvZero(unsigned width);
  BitwuzlaTermHandle bvMinusOne(unsigned width);
  BitwuzlaTermHandle bvConst32(unsigned width, uint32_t value);
  BitwuzlaTermHandle bvConst64(unsigned width, uint64_t value);
  BitwuzlaTermHandle bvZExtConst(unsigned width, uint64_t value);
  BitwuzlaTermHandle bvSExtConst(unsigned width, uint64_t value);
  BitwuzlaTermHandle bvBoolExtract(BitwuzlaTermHandle expr, int bit);
  BitwuzlaTermHandle bvExtract(BitwuzlaTermHandle expr, unsigned top,
                               unsigned bottom);
  BitwuzlaTermHandle eqExpr(BitwuzlaTermHandle a, BitwuzlaTermHandle b);

  BitwuzlaTermHandle bvLeftShift(BitwuzlaTermHandle expr, unsigned shift);
  BitwuzlaTermHandle bvRightShift(BitwuzlaTermHandle expr, unsigned shift);
  BitwuzlaTermHandle bvVarLeftShift(BitwuzlaTermHandle expr,
                                    BitwuzlaTermHandle shift);
  BitwuzlaTermHandle bvVarRightShift(BitwuzlaTermHandle expr,
                                     BitwuzlaTermHandle shift);
  BitwuzlaTermHandle bvVarArithRightShift(BitwuzlaTermHandle expr,
                                          BitwuzlaTermHandle shift);

  BitwuzlaTermHandle notExpr(BitwuzlaTermHandle expr);
  BitwuzlaTermHandle andExpr(BitwuzlaTermHandle lhs, BitwuzlaTermHandle rhs);
  BitwuzlaTermHandle orExpr(BitwuzlaTermHandle lhs, BitwuzlaTermHandle rhs);
  BitwuzlaTermHandle iffExpr(BitwuzlaTermHandle lhs, BitwuzlaTermHandle rhs);
  BitwuzlaTermHandle iteExpr(BitwuzlaTermHandle cond, BitwuzlaTermHandle t,
                             BitwuzlaTermHandle f);

  BitwuzlaTermHandle writeExpr(BitwuzlaTermHandle array,
                               BitwuzlaTermHandle index,
                               BitwuzlaTermHandle value);
  BitwuzlaTermHandle readExpr(BitwuzlaTermHandle array,
                              BitwuzlaTermHandle index);

  unsigned getBVLength(BitwuzlaTermHandle expr);

  BitwuzlaTermHandle constructAShrByConstant(BitwuzlaTermHandle expr,
                                             unsigned shift,
                                             BitwuzlaTermHandle isSigned);

  BitwuzlaTermHandle getInitialArray(const Array *os);
  BitwuzlaTermHandle getArrayForUpdate(const Array *root, const UpdateNode *un);

  BitwuzlaTermHandle constructActual(ref<Expr> e, int *width_out);
  BitwuzlaTermHandle construct(ref<Expr> e, int *width_out);

  BitwuzlaTermHandle buildArray(const char *name, unsigned indexWidth,
                                unsigned valueWidth);

  ::BitwuzlaSort getBvSort(unsigned width);
  ::BitwuzlaSort getArraySort(::BitwuzlaSort domain, ::BitwuzlaSort range);
  ::BitwuzlaSort getFloatSortFromBitWidth(unsigned bitWidth);

  // Float support. As in the Z3 builder, expressions are carried as bitvectors
  // and converted to a floating-point sort only where an operation needs one.
  static bool isFloat(::BitwuzlaTerm e);
  static void getFloatFormatFromBitWidth(unsigned bitWidth, unsigned &expBits,
                                         unsigned &sigBits);
  BitwuzlaTermHandle getCanonicalNaNBits(unsigned floatWidth);
  BitwuzlaTermHandle castToFloat(BitwuzlaTermHandle e);
  BitwuzlaTermHandle castToBitVector(BitwuzlaTermHandle e);
  BitwuzlaTermHandle getRoundingModeTerm(llvm::APFloat::roundingMode rm);
  BitwuzlaTermHandle
  getx87FP80ExplicitSignificandIntegerBit(BitwuzlaTermHandle e);
  BitwuzlaTermHandle getFreshBitVectorVariable(unsigned bitWidth,
                                               const char *prefix);

  bool autoClearConstructCache;

public:
  ::BitwuzlaTermManager *tm;

  BitwuzlaBuilder(bool autoClearConstructCache = true);
  ~BitwuzlaBuilder();

  BitwuzlaTermHandle getTrue();
  BitwuzlaTermHandle getFalse();
  BitwuzlaTermHandle getInitialRead(const Array *os, unsigned index);

  BitwuzlaTermHandle construct(ref<Expr> e) {
    BitwuzlaTermHandle res = construct(e, 0);
    if (autoClearConstructCache)
      clearConstructCache();
    return res;
  }

  void clearConstructCache() { constructed.clear(); }
  void clearSideConstraints() {
    sideConstraints.clear();
    floatToBitVectorVars.clear();
  }
};
}

#endif
