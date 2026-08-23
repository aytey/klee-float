//===-- BitwuzlaBuilder.cpp -----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "klee/Config/config.h"
#ifdef ENABLE_BITWUZLA

#include "BitwuzlaBuilder.h"

#include "klee/Expr.h"
#include "klee/Solver.h"
#include "klee/util/Bits.h"
#include "klee/SolverStats.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdio>

using namespace klee;

namespace {
llvm::cl::opt<bool> UseConstructHashBitwuzla(
    "use-construct-hash-bitwuzla",
    llvm::cl::desc(
        "Use hash-consing during Bitwuzla query construction (default=on)"),
    llvm::cl::init(true));
}

BitwuzlaArrayExprHash::~BitwuzlaArrayExprHash() {}

void BitwuzlaArrayExprHash::clear() {
  _update_node_hash.clear();
  _array_hash.clear();
}

void BitwuzlaArrayExprHash::clearUpdates() { _update_node_hash.clear(); }

BitwuzlaBuilder::BitwuzlaBuilder(bool autoClearConstructCache)
    : freshVariableCounter(0),
      autoClearConstructCache(autoClearConstructCache) {
  tm = bitwuzla_term_manager_new();
}

BitwuzlaBuilder::~BitwuzlaBuilder() {
  // Drop every reference we hold before tearing down the term manager they
  // belong to.
  clearConstructCache();
  clearSideConstraints();
  _arr_hash.clear();
  bitwuzla_term_manager_delete(tm);
}

/***/
/* Sorts.
 *
 * Sorts are hash-consed by Bitwuzla and drawn from a small set here (one per
 * bitvector width and float format in play), so these are not released; doing
 * so would only decrement a reference count on an object that is shared
 * anyway.
 */

::BitwuzlaSort BitwuzlaBuilder::getBvSort(unsigned width) {
  return bitwuzla_mk_bv_sort(tm, width);
}

::BitwuzlaSort BitwuzlaBuilder::getArraySort(::BitwuzlaSort domain,
                                             ::BitwuzlaSort range) {
  return bitwuzla_mk_array_sort(tm, domain, range);
}

::BitwuzlaSort BitwuzlaBuilder::getFloatSortFromBitWidth(unsigned bitWidth) {
  unsigned expBits, sigBits;
  getFloatFormatFromBitWidth(bitWidth, expBits, sigBits);
  return bitwuzla_mk_fp_sort(tm, expBits, sigBits);
}

void BitwuzlaBuilder::getFloatFormatFromBitWidth(unsigned bitWidth,
                                                 unsigned &expBits,
                                                 unsigned &sigBits) {
  switch (bitWidth) {
  case Expr::Int16: expBits = 5;  sigBits = 11;  return;
  case Expr::Int32: expBits = 8;  sigBits = 24;  return;
  case Expr::Int64: expBits = 11; sigBits = 53;  return;
  case Expr::Fl80:
    // An IEEE-754 format with a 15 bit exponent and a 64 bit significand.
    // That is not x87 fp80's binary encoding -- see castToFloat() -- but it
    // has the right precision.
    expBits = 15; sigBits = 64; return;
  case Expr::Int128: expBits = 15; sigBits = 113; return;
  default:
    llvm_unreachable("bitWidth cannot be converted to an IEEE-754 binary-* "
                     "number");
  }
}

/***/
/* Constants and basic terms */

BitwuzlaTermHandle BitwuzlaBuilder::getTrue() {
  return BitwuzlaTermHandle(bitwuzla_mk_true(tm));
}

BitwuzlaTermHandle BitwuzlaBuilder::getFalse() {
  return BitwuzlaTermHandle(bitwuzla_mk_false(tm));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvOne(unsigned width) {
  return bvZExtConst(width, 1);
}

BitwuzlaTermHandle BitwuzlaBuilder::bvZero(unsigned width) {
  return bvZExtConst(width, 0);
}

BitwuzlaTermHandle BitwuzlaBuilder::bvMinusOne(unsigned width) {
  return bvSExtConst(width, (int64_t)-1);
}

/// Bitwuzla rejects a value that does not fit the sort (Z3 silently truncates),
/// so narrow it here -- bvMinusOne(), for one, asks for ~0 at every width.
static uint64_t truncateToWidth(uint64_t value, unsigned width) {
  if (width >= 64)
    return value;
  return value & ((((uint64_t)1) << width) - 1);
}

BitwuzlaTermHandle BitwuzlaBuilder::bvConst32(unsigned width, uint32_t value) {
  return BitwuzlaTermHandle(bitwuzla_mk_bv_value_uint64(
      tm, getBvSort(width), truncateToWidth(value, width)));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvConst64(unsigned width, uint64_t value) {
  return BitwuzlaTermHandle(bitwuzla_mk_bv_value_uint64(
      tm, getBvSort(width), truncateToWidth(value, width)));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvZExtConst(unsigned width,
                                                uint64_t value) {
  if (width <= 64)
    return bvConst64(width, value);
  return BitwuzlaTermHandle(
      bitwuzla_mk_term1_indexed1(tm, BITWUZLA_KIND_BV_ZERO_EXTEND,
                                 bvConst64(64, value), width - 64));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvSExtConst(unsigned width,
                                                uint64_t value) {
  if (width <= 64)
    return bvConst64(width, value);
  return BitwuzlaTermHandle(bitwuzla_mk_term1_indexed1(
      tm, BITWUZLA_KIND_BV_SIGN_EXTEND, bvConst64(64, value), width - 64));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvBoolExtract(BitwuzlaTermHandle expr,
                                                  int bit) {
  return eqExpr(bvExtract(expr, bit, bit), bvOne(1));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvExtract(BitwuzlaTermHandle expr,
                                              unsigned top, unsigned bottom) {
  return BitwuzlaTermHandle(bitwuzla_mk_term1_indexed2(
      tm, BITWUZLA_KIND_BV_EXTRACT, castToBitVector(expr), top, bottom));
}

BitwuzlaTermHandle BitwuzlaBuilder::eqExpr(BitwuzlaTermHandle a,
                                           BitwuzlaTermHandle b) {
  // Handle implicit bitvector/float coercion
  if (isFloat(a) != isFloat(b)) {
    a = castToBitVector(a);
    b = castToBitVector(b);
  }
  return BitwuzlaTermHandle(bitwuzla_mk_term2(tm, BITWUZLA_KIND_EQUAL, a, b));
}

BitwuzlaTermHandle BitwuzlaBuilder::notExpr(BitwuzlaTermHandle expr) {
  return BitwuzlaTermHandle(bitwuzla_mk_term1(tm, BITWUZLA_KIND_NOT, expr));
}

BitwuzlaTermHandle BitwuzlaBuilder::andExpr(BitwuzlaTermHandle lhs,
                                            BitwuzlaTermHandle rhs) {
  return BitwuzlaTermHandle(
      bitwuzla_mk_term2(tm, BITWUZLA_KIND_AND, lhs, rhs));
}

BitwuzlaTermHandle BitwuzlaBuilder::orExpr(BitwuzlaTermHandle lhs,
                                           BitwuzlaTermHandle rhs) {
  return BitwuzlaTermHandle(bitwuzla_mk_term2(tm, BITWUZLA_KIND_OR, lhs, rhs));
}

BitwuzlaTermHandle BitwuzlaBuilder::iffExpr(BitwuzlaTermHandle lhs,
                                            BitwuzlaTermHandle rhs) {
  return BitwuzlaTermHandle(bitwuzla_mk_term2(tm, BITWUZLA_KIND_IFF, lhs, rhs));
}

BitwuzlaTermHandle BitwuzlaBuilder::iteExpr(BitwuzlaTermHandle cond,
                                            BitwuzlaTermHandle whenTrue,
                                            BitwuzlaTermHandle whenFalse) {
  // Handle implicit bitvector/float coercion
  if (isFloat(whenTrue) != isFloat(whenFalse)) {
    whenTrue = castToBitVector(whenTrue);
    whenFalse = castToBitVector(whenFalse);
  }
  return BitwuzlaTermHandle(
      bitwuzla_mk_term3(tm, BITWUZLA_KIND_ITE, cond, whenTrue, whenFalse));
}

BitwuzlaTermHandle BitwuzlaBuilder::writeExpr(BitwuzlaTermHandle array,
                                              BitwuzlaTermHandle index,
                                              BitwuzlaTermHandle value) {
  return BitwuzlaTermHandle(bitwuzla_mk_term3(
      tm, BITWUZLA_KIND_ARRAY_STORE, array, index, castToBitVector(value)));
}

BitwuzlaTermHandle BitwuzlaBuilder::readExpr(BitwuzlaTermHandle array,
                                             BitwuzlaTermHandle index) {
  return BitwuzlaTermHandle(
      bitwuzla_mk_term2(tm, BITWUZLA_KIND_ARRAY_SELECT, array, index));
}

unsigned BitwuzlaBuilder::getBVLength(BitwuzlaTermHandle expr) {
  return bitwuzla_sort_bv_get_size(bitwuzla_term_get_sort(expr));
}

/***/
/* Shifts */

BitwuzlaTermHandle BitwuzlaBuilder::bvLeftShift(BitwuzlaTermHandle expr,
                                                unsigned shift) {
  BitwuzlaTermHandle exprAsBv = castToBitVector(expr);
  unsigned width = getBVLength(exprAsBv);
  if (shift == 0)
    return exprAsBv;
  if (shift >= width)
    return bvZero(width);
  return BitwuzlaTermHandle(
      bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT,
                        bvExtract(exprAsBv, width - shift - 1, 0),
                        bvZero(shift)));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvRightShift(BitwuzlaTermHandle expr,
                                                 unsigned shift) {
  BitwuzlaTermHandle exprAsBv = castToBitVector(expr);
  unsigned width = getBVLength(exprAsBv);
  if (shift == 0)
    return exprAsBv;
  if (shift >= width)
    return bvZero(width);
  return BitwuzlaTermHandle(
      bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT, bvZero(shift),
                        bvExtract(exprAsBv, width - 1, shift)));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvVarLeftShift(BitwuzlaTermHandle expr,
                                                   BitwuzlaTermHandle shift) {
  return BitwuzlaTermHandle(bitwuzla_mk_term2(
      tm, BITWUZLA_KIND_BV_SHL, castToBitVector(expr), castToBitVector(shift)));
}

BitwuzlaTermHandle BitwuzlaBuilder::bvVarRightShift(BitwuzlaTermHandle expr,
                                                    BitwuzlaTermHandle shift) {
  return BitwuzlaTermHandle(bitwuzla_mk_term2(
      tm, BITWUZLA_KIND_BV_SHR, castToBitVector(expr), castToBitVector(shift)));
}

BitwuzlaTermHandle
BitwuzlaBuilder::bvVarArithRightShift(BitwuzlaTermHandle expr,
                                      BitwuzlaTermHandle shift) {
  BitwuzlaTermHandle exprAsBv = castToBitVector(expr);
  BitwuzlaTermHandle shiftAsBv = castToBitVector(shift);
  unsigned width = getBVLength(exprAsBv);
  // bvashr saturates to the sign bit when overshifting, but KLEE defines an
  // overshift as zero (see constructAShrByConstant()), so guard it.
  BitwuzlaTermHandle inRange(bitwuzla_mk_term2(
      tm, BITWUZLA_KIND_BV_ULT, shiftAsBv,
      bvConst32(getBVLength(shiftAsBv), width)));
  return iteExpr(inRange,
                 BitwuzlaTermHandle(bitwuzla_mk_term2(
                     tm, BITWUZLA_KIND_BV_ASHR, exprAsBv, shiftAsBv)),
                 bvZero(width));
}

BitwuzlaTermHandle
BitwuzlaBuilder::constructAShrByConstant(BitwuzlaTermHandle expr,
                                         unsigned shift,
                                         BitwuzlaTermHandle isSigned) {
  BitwuzlaTermHandle exprAsBv = castToBitVector(expr);
  unsigned width = getBVLength(exprAsBv);
  if (shift == 0)
    return exprAsBv;
  if (shift >= width)
    return bvZero(width); // Overshift to zero
  return iteExpr(isSigned,
                 BitwuzlaTermHandle(bitwuzla_mk_term2(
                     tm, BITWUZLA_KIND_BV_CONCAT, bvMinusOne(shift),
                     bvExtract(exprAsBv, width - 1, shift))),
                 bvRightShift(exprAsBv, shift));
}

/***/
/* Floating point */

bool BitwuzlaBuilder::isFloat(::BitwuzlaTerm e) {
  return bitwuzla_sort_is_fp(bitwuzla_term_get_sort(e));
}

BitwuzlaTermHandle
BitwuzlaBuilder::getFreshBitVectorVariable(unsigned bitWidth,
                                           const char *prefix) {
  char name[64];
  snprintf(name, sizeof(name), "%s%u", prefix, freshVariableCounter++);
  return BitwuzlaTermHandle(
      bitwuzla_mk_const(tm, getBvSort(bitWidth), name));
}

BitwuzlaTermHandle
BitwuzlaBuilder::getx87FP80ExplicitSignificandIntegerBit(
    BitwuzlaTermHandle e) {
#ifndef NDEBUG
  ::BitwuzlaSort sort = bitwuzla_term_get_sort(e);
  assert(bitwuzla_sort_is_fp(sort));
  assert(bitwuzla_sort_fp_get_exp_size(sort) == 15);
  assert(bitwuzla_sort_fp_get_sig_size(sort) == 64);
#endif
  // If the number is a denormal or zero then the implicit integer bit is zero,
  // otherwise it is one.
  BitwuzlaTermHandle isDenormal(
      bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_SUBNORMAL, e));
  BitwuzlaTermHandle isZero(
      bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_ZERO, e));
  return BitwuzlaTermHandle(
      bitwuzla_mk_term3(tm, BITWUZLA_KIND_ITE, orExpr(isDenormal, isZero),
                        bvZero(1), bvOne(1)));
}

/// The bit pattern KLEE's own constant folding produces for a NaN of this
/// format (ConstantExpr::GetNaN(), enabled by --single-repr-for-nan), in the
/// IEEE layout -- i.e. for the format modelling x87 fp80 this is the 79 bit
/// encoding, without the explicit significand integer bit.
BitwuzlaTermHandle BitwuzlaBuilder::getCanonicalNaNBits(unsigned floatWidth) {
  switch (floatWidth) {
  case Expr::Int16:
    return bvConst64(16, 0x7c01);
  case Expr::Int32:
    return bvConst64(32, 0x7f800001);
  case Expr::Int64:
    return bvConst64(64, 0x7ff0000000000001ULL);
  case 79:
    // sign 0, exponent all ones, trailing significand 1
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT, bvConst64(16, 0x7fff),
                          bvZExtConst(63, 1)));
  case Expr::Int128:
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT, bvConst64(16, 0x7fff),
                          bvZExtConst(112, 1)));
  default:
    llvm_unreachable("no canonical NaN for this width");
  }
}

BitwuzlaTermHandle BitwuzlaBuilder::castToFloat(BitwuzlaTermHandle e) {
  if (isFloat(e))
    return e;

  unsigned bitWidth = getBVLength(e);
  unsigned expBits, sigBits;
  switch (bitWidth) {
  case Expr::Int16:
  case Expr::Int32:
  case Expr::Int64:
  case Expr::Int128:
    getFloatFormatFromBitWidth(bitWidth, expBits, sigBits);
    return BitwuzlaTermHandle(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_BV, e, expBits, sigBits));
  case Expr::Fl80: {
    // x87 fp80 stores the significand's integer bit explicitly, so its 80 bit
    // encoding is
    //
    //   Sign Exponent Significand
    //   [1]    [15]   [1] [63]
    //
    // whereas the 79 bit IEEE-754 format we model it with leaves that bit
    // implicit
    //
    //   Sign Exponent [Significand]
    //   [1]    [15]       [63]
    //
    // Both have exponent bias 16383. Repack, and emit a side constraint fixing
    // the explicit bit so that a model round-trips to a valid x87 fp80.
    //
    // This assumes IEEE semantics; x87 fp80 has further semantics arising from
    // the explicit bit (see 8.2.2 "Unsupported Double Extended-Precision
    // Floating-Point Encodings and Pseudo-Denormals" in the Intel 64 and IA-32
    // Architectures Software Developer's Manual) which this cannot model.
    //
    // Keep in sync with castToBitVector(), which inverts this.
    BitwuzlaTermHandle signBit(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, e, 79, 79));
    BitwuzlaTermHandle exponentBits(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, e, 78, 64));
    BitwuzlaTermHandle significandIntegerBit(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, e, 63, 63));
    BitwuzlaTermHandle significandFractionBits(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, e, 62, 0));

    BitwuzlaTermHandle ieeeBitPattern(bitwuzla_mk_term2(
        tm, BITWUZLA_KIND_BV_CONCAT, signBit, exponentBits));
    ieeeBitPattern = BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT, ieeeBitPattern,
                          significandFractionBits));
    assert(getBVLength(ieeeBitPattern) == 79);

    getFloatFormatFromBitWidth(bitWidth, expBits, sigBits);
    BitwuzlaTermHandle asFloat(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_BV, ieeeBitPattern, expBits, sigBits));

    sideConstraints.push_back(
        eqExpr(significandIntegerBit,
               getx87FP80ExplicitSignificandIntegerBit(asFloat)));
    return asFloat;
  }
  default:
    llvm_unreachable("Unhandled width when casting bitvector to float");
  }
}

BitwuzlaTermHandle BitwuzlaBuilder::castToBitVector(BitwuzlaTermHandle e) {
  if (!isFloat(e))
    return e;

  ::BitwuzlaSort currentSort = bitwuzla_term_get_sort(e);
  unsigned exponentBits = bitwuzla_sort_fp_get_exp_size(currentSort);
  unsigned significandBits = bitwuzla_sort_fp_get_sig_size(currentSort);
  unsigned floatWidth = exponentBits + significandBits;

  // Bitwuzla implements the SMT-LIB floating-point theory, which has no
  // float-to-bits operation -- Z3's fp.to_ieee_bv is an extension. So
  // introduce a bitvector variable and constrain it to reinterpret as this
  // float. Note the constraint does not pin the bits of a NaN, since every NaN
  // encoding reinterprets to the one NaN value; as with Z3's canonicalising
  // fp.to_ieee_bv, castToBitVector(castToFloat(e)) need not equal e for NaN.
  //
  // The variable is cached per float term so that casting the same float twice
  // cannot yield two variables a model could give different bits.
  BitwuzlaTermHandle ieeeBits;
  std::map< ::BitwuzlaTerm, BitwuzlaTermHandle>::iterator it =
      floatToBitVectorVars.find((::BitwuzlaTerm)e);
  if (it != floatToBitVectorVars.end()) {
    ieeeBits = it->second;
  } else {
    ieeeBits = getFreshBitVectorVariable(floatWidth, "__klee_fp_bits_");
    BitwuzlaTermHandle reinterpreted(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_BV, ieeeBits, exponentBits,
        significandBits));
    sideConstraints.push_back(BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_EQUAL, reinterpreted, e)));
    // Every NaN encoding reinterprets to the one NaN value, so the constraint
    // above leaves the bits of a NaN free. KLEE's own constant folding yields
    // one specific encoding (ConstantExpr::GetNaN()), and Z3's fp.to_ieee_bv
    // canonicalises to match it, so a model that picked any other encoding
    // would disagree with how KLEE evaluates the same expression. Pin it.
    sideConstraints.push_back(BitwuzlaTermHandle(bitwuzla_mk_term2(
        tm, BITWUZLA_KIND_IMPLIES,
        BitwuzlaTermHandle(
            bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_NAN, e)),
        BitwuzlaTermHandle(bitwuzla_mk_term2(
            tm, BITWUZLA_KIND_EQUAL, ieeeBits,
            getCanonicalNaNBits(floatWidth))))));
    floatToBitVectorVars.insert(std::make_pair((::BitwuzlaTerm)e, ieeeBits));
  }

  switch (floatWidth) {
  case Expr::Int16:
  case Expr::Int32:
  case Expr::Int64:
  case Expr::Int128:
    return ieeeBits;
  case 79: {
    // Expr::Fl80. The widths sum to 79 rather than 80 because x87's "implicit"
    // significand bit is not actually implicit; reinsert it.
    BitwuzlaTermHandle signBit(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, ieeeBits, 78, 78));
    BitwuzlaTermHandle expBits(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, ieeeBits, 77, 63));
    BitwuzlaTermHandle significandIntegerBit =
        getx87FP80ExplicitSignificandIntegerBit(e);
    BitwuzlaTermHandle fractionBits(bitwuzla_mk_term1_indexed2(
        tm, BITWUZLA_KIND_BV_EXTRACT, ieeeBits, 62, 0));

    BitwuzlaTermHandle x87Bits(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT, signBit, expBits));
    x87Bits = BitwuzlaTermHandle(bitwuzla_mk_term2(
        tm, BITWUZLA_KIND_BV_CONCAT, x87Bits, significandIntegerBit));
    x87Bits = BitwuzlaTermHandle(bitwuzla_mk_term2(
        tm, BITWUZLA_KIND_BV_CONCAT, x87Bits, fractionBits));
    assert(getBVLength(x87Bits) == 80);
    return x87Bits;
  }
  default:
    llvm_unreachable("Unhandled width when casting float to bitvector");
  }
}

BitwuzlaTermHandle
BitwuzlaBuilder::getRoundingModeTerm(llvm::APFloat::roundingMode rm) {
  switch (rm) {
  case llvm::APFloat::rmNearestTiesToEven:
    return BitwuzlaTermHandle(bitwuzla_mk_rm_value(tm, BITWUZLA_RM_RNE));
  case llvm::APFloat::rmTowardPositive:
    return BitwuzlaTermHandle(bitwuzla_mk_rm_value(tm, BITWUZLA_RM_RTP));
  case llvm::APFloat::rmTowardNegative:
    return BitwuzlaTermHandle(bitwuzla_mk_rm_value(tm, BITWUZLA_RM_RTN));
  case llvm::APFloat::rmTowardZero:
    return BitwuzlaTermHandle(bitwuzla_mk_rm_value(tm, BITWUZLA_RM_RTZ));
  case llvm::APFloat::rmNearestTiesToAway:
    return BitwuzlaTermHandle(bitwuzla_mk_rm_value(tm, BITWUZLA_RM_RNA));
  default:
    llvm_unreachable("Unhandled rounding mode");
  }
}

/***/
/* Arrays */

BitwuzlaTermHandle BitwuzlaBuilder::buildArray(const char *name,
                                               unsigned indexWidth,
                                               unsigned valueWidth) {
  ::BitwuzlaSort domain = getBvSort(indexWidth);
  ::BitwuzlaSort range = getBvSort(valueWidth);
  return BitwuzlaTermHandle(
      bitwuzla_mk_const(tm, getArraySort(domain, range), name));
}

BitwuzlaTermHandle BitwuzlaBuilder::getInitialArray(const Array *root) {
  assert(root);
  BitwuzlaTermHandle array_expr;
  bool hashed = _arr_hash.lookupArrayExpr(root, array_expr);

  if (!hashed) {
    // Unique arrays by name, so we make sure the name is unique by using the
    // size of the array hash as a counter.
    std::string unique_id = llvm::itostr(_arr_hash._array_hash.size());
    unsigned const uid_length = unique_id.length();
    unsigned const space = (root->name.length() > 32 - uid_length)
                               ? (32 - uid_length)
                               : root->name.length();
    std::string unique_name = root->name.substr(0, space) + unique_id;

    array_expr =
        buildArray(unique_name.c_str(), root->getDomain(), root->getRange());

    if (root->isConstantArray()) {
      for (unsigned i = 0, e = root->size; i != e; ++i) {
        BitwuzlaTermHandle prev = array_expr;
        array_expr = writeExpr(
            prev, construct(ConstantExpr::alloc(i, root->getDomain()), 0),
            construct(root->constantValues[i], 0));
      }
    }

    _arr_hash.hashArrayExpr(root, array_expr);
  }

  return array_expr;
}

BitwuzlaTermHandle BitwuzlaBuilder::getArrayForUpdate(const Array *root,
                                                      const UpdateNode *un) {
  if (!un)
    return getInitialArray(root);

  BitwuzlaTermHandle un_expr;
  bool hashed = _arr_hash.lookupUpdateNodeExpr(un, un_expr);
  if (!hashed) {
    un_expr = writeExpr(getArrayForUpdate(root, un->next),
                        construct(un->index, 0), construct(un->value, 0));
    _arr_hash.hashUpdateNodeExpr(un, un_expr);
  }
  return un_expr;
}

BitwuzlaTermHandle BitwuzlaBuilder::getInitialRead(const Array *root,
                                                   unsigned index) {
  return readExpr(getInitialArray(root), bvConst32(32, index));
}

/***/
/* Construction */

BitwuzlaTermHandle BitwuzlaBuilder::construct(ref<Expr> e, int *width_out) {
  if (!UseConstructHashBitwuzla || isa<ConstantExpr>(e))
    return constructActual(e, width_out);

  ExprHashMap<std::pair<BitwuzlaTermHandle, unsigned> >::iterator it =
      constructed.find(e);
  if (it != constructed.end()) {
    if (width_out)
      *width_out = it->second.second;
    return it->second.first;
  }

  int width;
  if (!width_out)
    width_out = &width;
  BitwuzlaTermHandle res = constructActual(e, width_out);
  constructed.insert(std::make_pair(e, std::make_pair(res, *width_out)));
  return res;
}

/** if *width_out!=1 then result is a bitvector, otherwise it is a bool */
BitwuzlaTermHandle BitwuzlaBuilder::constructActual(ref<Expr> e,
                                                    int *width_out) {
  int width;
  if (!width_out)
    width_out = &width;

  ++stats::queryConstructs;

  switch (e->getKind()) {
  case Expr::Constant: {
    ConstantExpr *CE = cast<ConstantExpr>(e);
    *width_out = CE->getWidth();

    if (*width_out == 1)
      return CE->isTrue() ? getTrue() : getFalse();

    BitwuzlaTermHandle Res;
    if (*width_out <= 32) {
      Res = bvConst32(*width_out, CE->getZExtValue(32));
    } else if (*width_out <= 64) {
      Res = bvConst64(*width_out, CE->getZExtValue());
    } else {
      ref<ConstantExpr> Tmp = CE;
      Res = bvConst64(64, Tmp->Extract(0, 64)->getZExtValue());
      while (Tmp->getWidth() > 64) {
        Tmp = Tmp->Extract(64, Tmp->getWidth() - 64);
        unsigned Width = std::min(64U, Tmp->getWidth());
        Res = BitwuzlaTermHandle(bitwuzla_mk_term2(
            tm, BITWUZLA_KIND_BV_CONCAT,
            bvConst64(Width, Tmp->Extract(0, Width)->getZExtValue()), Res));
      }
    }

    // Coerce to float if necessary
    if (CE->isFloat())
      Res = castToFloat(Res);
    return Res;
  }

  case Expr::NotOptimized: {
    NotOptimizedExpr *noe = cast<NotOptimizedExpr>(e);
    return construct(noe->src, width_out);
  }

  case Expr::Read: {
    ReadExpr *re = cast<ReadExpr>(e);
    assert(re && re->updates.root);
    *width_out = re->updates.root->getRange();
    return readExpr(getArrayForUpdate(re->updates.root, re->updates.head),
                    construct(re->index, 0));
  }

  case Expr::Select: {
    SelectExpr *se = cast<SelectExpr>(e);
    BitwuzlaTermHandle cond = construct(se->cond, 0);
    BitwuzlaTermHandle tExpr = construct(se->trueExpr, width_out);
    BitwuzlaTermHandle fExpr = construct(se->falseExpr, width_out);
    return iteExpr(cond, tExpr, fExpr);
  }

  case Expr::Concat: {
    ConcatExpr *ce = cast<ConcatExpr>(e);
    unsigned numKids = ce->getNumKids();
    BitwuzlaTermHandle res =
        castToBitVector(construct(ce->getKid(numKids - 1), 0));
    for (int i = numKids - 2; i >= 0; i--) {
      res = BitwuzlaTermHandle(
          bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_CONCAT,
                            castToBitVector(construct(ce->getKid(i), 0)), res));
    }
    *width_out = ce->getWidth();
    return res;
  }

  case Expr::Extract: {
    ExtractExpr *ee = cast<ExtractExpr>(e);
    BitwuzlaTermHandle src = construct(ee->expr, width_out);
    *width_out = ee->getWidth();
    if (*width_out == 1)
      return bvBoolExtract(src, ee->offset);
    return bvExtract(src, ee->offset + *width_out - 1, ee->offset);
  }

  // Casting

  case Expr::ZExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    BitwuzlaTermHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth == 1)
      return iteExpr(src, bvOne(*width_out), bvZero(*width_out));
    assert(*width_out > srcWidth && "Invalid width_out");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1_indexed1(tm, BITWUZLA_KIND_BV_ZERO_EXTEND,
                                   castToBitVector(src),
                                   *width_out - srcWidth));
  }

  case Expr::SExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    BitwuzlaTermHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth == 1)
      return iteExpr(src, bvMinusOne(*width_out), bvZero(*width_out));
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1_indexed1(tm, BITWUZLA_KIND_BV_SIGN_EXTEND,
                                   castToBitVector(src),
                                   *width_out - srcWidth));
  }

  case Expr::FPExt: {
    int srcWidth;
    FPExtExpr *ce = cast<FPExtExpr>(e);
    BitwuzlaTermHandle src = castToFloat(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(*width_out)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid FPExt width");
    assert(*width_out >= srcWidth && "Invalid FPExt");
    unsigned expBits, sigBits;
    getFloatFormatFromBitWidth(*width_out, expBits, sigBits);
    // Any rounding mode will do here as we are extending.
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_FP,
        getRoundingModeTerm(llvm::APFloat::rmNearestTiesToEven), src, expBits,
        sigBits));
  }

  case Expr::FPTrunc: {
    int srcWidth;
    FPTruncExpr *ce = cast<FPTruncExpr>(e);
    BitwuzlaTermHandle src = castToFloat(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(*width_out)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid FPTrunc width");
    assert(*width_out <= srcWidth && "Invalid FPTrunc");
    unsigned expBits, sigBits;
    getFloatFormatFromBitWidth(*width_out, expBits, sigBits);
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_FP,
        getRoundingModeTerm(ce->roundingMode), src, expBits, sigBits));
  }

  case Expr::FPToUI: {
    int srcWidth;
    FPToUIExpr *ce = cast<FPToUIExpr>(e);
    BitwuzlaTermHandle src = castToFloat(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(srcWidth)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid FPToUI width");
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed1(
        tm, BITWUZLA_KIND_FP_TO_UBV, getRoundingModeTerm(ce->roundingMode),
        src, *width_out));
  }

  case Expr::FPToSI: {
    int srcWidth;
    FPToSIExpr *ce = cast<FPToSIExpr>(e);
    BitwuzlaTermHandle src = castToFloat(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(srcWidth)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid FPToSI width");
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed1(
        tm, BITWUZLA_KIND_FP_TO_SBV, getRoundingModeTerm(ce->roundingMode),
        src, *width_out));
  }

  case Expr::UIToFP: {
    int srcWidth;
    UIToFPExpr *ce = cast<UIToFPExpr>(e);
    BitwuzlaTermHandle src = castToBitVector(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(*width_out)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid UIToFP width");
    unsigned expBits, sigBits;
    getFloatFormatFromBitWidth(*width_out, expBits, sigBits);
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_UBV,
        getRoundingModeTerm(ce->roundingMode), src, expBits, sigBits));
  }

  case Expr::SIToFP: {
    int srcWidth;
    SIToFPExpr *ce = cast<SIToFPExpr>(e);
    BitwuzlaTermHandle src = castToBitVector(construct(ce->src, &srcWidth));
    *width_out = ce->getWidth();
    assert(&(ConstantExpr::widthToFloatSemantics(*width_out)) !=
               &(llvm::APFloat::Bogus) &&
           "Invalid SIToFP width");
    unsigned expBits, sigBits;
    getFloatFormatFromBitWidth(*width_out, expBits, sigBits);
    return BitwuzlaTermHandle(bitwuzla_mk_term2_indexed2(
        tm, BITWUZLA_KIND_FP_TO_FP_FROM_SBV,
        getRoundingModeTerm(ce->roundingMode), src, expBits, sigBits));
  }

  // Arithmetic

  case Expr::Add: {
    AddExpr *ae = cast<AddExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(ae->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(ae->right, width_out));
    assert(*width_out != 1 && "uncanonicalized add");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_ADD, left, right));
  }

  case Expr::Sub: {
    SubExpr *se = cast<SubExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(se->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(se->right, width_out));
    assert(*width_out != 1 && "uncanonicalized sub");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_SUB, left, right));
  }

  case Expr::Mul: {
    MulExpr *me = cast<MulExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(me->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(me->right, width_out));
    assert(*width_out != 1 && "uncanonicalized mul");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_MUL, left, right));
  }

  case Expr::UDiv: {
    UDivExpr *de = cast<UDivExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(de->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(de->right, width_out));
    assert(*width_out != 1 && "uncanonicalized udiv");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_UDIV, left, right));
  }

  case Expr::SDiv: {
    SDivExpr *de = cast<SDivExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(de->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(de->right, width_out));
    assert(*width_out != 1 && "uncanonicalized sdiv");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_SDIV, left, right));
  }

  case Expr::URem: {
    URemExpr *de = cast<URemExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(de->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(de->right, width_out));
    assert(*width_out != 1 && "uncanonicalized urem");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_UREM, left, right));
  }

  case Expr::SRem: {
    SRemExpr *de = cast<SRemExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(de->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(de->right, width_out));
    assert(*width_out != 1 && "uncanonicalized srem");
    // LLVM's srem instruction and SMT-LIB's bvsrem both take the sign of the
    // dividend, unlike bvsmod.
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_SREM, left, right));
  }

  // Bitwise

  case Expr::Not: {
    NotExpr *ne = cast<NotExpr>(e);
    BitwuzlaTermHandle expr = construct(ne->expr, width_out);
    if (*width_out == 1)
      return notExpr(expr);
    return BitwuzlaTermHandle(bitwuzla_mk_term1(tm, BITWUZLA_KIND_BV_NOT,
                                                castToBitVector(expr)));
  }

  case Expr::And: {
    AndExpr *ae = cast<AndExpr>(e);
    BitwuzlaTermHandle left = construct(ae->left, width_out);
    BitwuzlaTermHandle right = construct(ae->right, width_out);
    if (*width_out == 1)
      return andExpr(left, right);
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_AND, castToBitVector(left),
                          castToBitVector(right)));
  }

  case Expr::Or: {
    OrExpr *oe = cast<OrExpr>(e);
    BitwuzlaTermHandle left = construct(oe->left, width_out);
    BitwuzlaTermHandle right = construct(oe->right, width_out);
    if (*width_out == 1)
      return orExpr(left, right);
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_OR, castToBitVector(left),
                          castToBitVector(right)));
  }

  case Expr::Xor: {
    XorExpr *xe = cast<XorExpr>(e);
    BitwuzlaTermHandle left = construct(xe->left, width_out);
    BitwuzlaTermHandle right = construct(xe->right, width_out);
    if (*width_out == 1)
      return BitwuzlaTermHandle(
          bitwuzla_mk_term2(tm, BITWUZLA_KIND_XOR, left, right));
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_XOR, castToBitVector(left),
                          castToBitVector(right)));
  }

  case Expr::Shl: {
    ShlExpr *se = cast<ShlExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(se->left, width_out));
    assert(*width_out != 1 && "uncanonicalized shl");
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(se->right))
      return bvLeftShift(left, (unsigned)CE->getLimitedValue());
    int shiftWidth;
    BitwuzlaTermHandle amount =
        castToBitVector(construct(se->right, &shiftWidth));
    return bvVarLeftShift(left, amount);
  }

  case Expr::LShr: {
    LShrExpr *lse = cast<LShrExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(lse->left, width_out));
    assert(*width_out != 1 && "uncanonicalized lshr");
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(lse->right))
      return bvRightShift(left, (unsigned)CE->getLimitedValue());
    int shiftWidth;
    BitwuzlaTermHandle amount =
        castToBitVector(construct(lse->right, &shiftWidth));
    return bvVarRightShift(left, amount);
  }

  case Expr::AShr: {
    AShrExpr *ase = cast<AShrExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(ase->left, width_out));
    assert(*width_out != 1 && "uncanonicalized ashr");
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ase->right)) {
      unsigned shift = (unsigned)CE->getLimitedValue();
      BitwuzlaTermHandle signedBool = bvBoolExtract(left, *width_out - 1);
      return constructAShrByConstant(left, shift, signedBool);
    }
    int shiftWidth;
    BitwuzlaTermHandle amount =
        castToBitVector(construct(ase->right, &shiftWidth));
    return bvVarArithRightShift(left, amount);
  }

  // Comparison

  case Expr::Eq: {
    EqExpr *ee = cast<EqExpr>(e);
    BitwuzlaTermHandle left = construct(ee->left, width_out);
    BitwuzlaTermHandle right = construct(ee->right, width_out);
    if (*width_out == 1) {
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ee->left)) {
        if (CE->isTrue())
          return right;
        return notExpr(right);
      }
      return iffExpr(left, right);
    }
    *width_out = 1;
    return eqExpr(left, right);
  }

  case Expr::Ult: {
    UltExpr *ue = cast<UltExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(ue->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(ue->right, width_out));
    assert(*width_out != 1 && "uncanonicalized ult");
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_ULT, left, right));
  }

  case Expr::Ule: {
    UleExpr *ue = cast<UleExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(ue->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(ue->right, width_out));
    assert(*width_out != 1 && "uncanonicalized ule");
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_ULE, left, right));
  }

  case Expr::Slt: {
    SltExpr *se = cast<SltExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(se->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(se->right, width_out));
    assert(*width_out != 1 && "uncanonicalized slt");
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_SLT, left, right));
  }

  case Expr::Sle: {
    SleExpr *se = cast<SleExpr>(e);
    BitwuzlaTermHandle left = castToBitVector(construct(se->left, width_out));
    BitwuzlaTermHandle right = castToBitVector(construct(se->right, width_out));
    assert(*width_out != 1 && "uncanonicalized sle");
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_BV_SLE, left, right));
  }

  // Floating point comparison

  case Expr::FOEq: {
    FOEqExpr *fcmp = cast<FOEqExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(fcmp->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(fcmp->right, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_FP_EQUAL, left, right));
  }

  case Expr::FOLt: {
    FOLtExpr *fcmp = cast<FOLtExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(fcmp->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(fcmp->right, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_FP_LT, left, right));
  }

  case Expr::FOLe: {
    FOLeExpr *fcmp = cast<FOLeExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(fcmp->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(fcmp->right, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_FP_LEQ, left, right));
  }

  case Expr::FOGt: {
    FOGtExpr *fcmp = cast<FOGtExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(fcmp->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(fcmp->right, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_FP_GT, left, right));
  }

  case Expr::FOGe: {
    FOGeExpr *fcmp = cast<FOGeExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(fcmp->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(fcmp->right, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term2(tm, BITWUZLA_KIND_FP_GEQ, left, right));
  }

  // Floating point predicates

  case Expr::IsNaN: {
    IsNaNExpr *ine = cast<IsNaNExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(ine->expr, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_NAN, arg));
  }

  case Expr::IsInfinite: {
    IsInfiniteExpr *iie = cast<IsInfiniteExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(iie->expr, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_INF, arg));
  }

  case Expr::IsNormal: {
    IsNormalExpr *ine = cast<IsNormalExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(ine->expr, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_NORMAL, arg));
  }

  case Expr::IsSubnormal: {
    IsSubnormalExpr *ise = cast<IsSubnormalExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(ise->expr, width_out));
    *width_out = 1;
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_IS_SUBNORMAL, arg));
  }

  // Floating point arithmetic

  case Expr::FAdd: {
    FAddExpr *f = cast<FAddExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(f->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(f->right, width_out));
    assert(*width_out != 1 && "uncanonicalized FAdd");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term3(tm, BITWUZLA_KIND_FP_ADD,
                          getRoundingModeTerm(f->roundingMode), left, right));
  }

  case Expr::FSub: {
    FSubExpr *f = cast<FSubExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(f->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(f->right, width_out));
    assert(*width_out != 1 && "uncanonicalized FSub");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term3(tm, BITWUZLA_KIND_FP_SUB,
                          getRoundingModeTerm(f->roundingMode), left, right));
  }

  case Expr::FMul: {
    FMulExpr *f = cast<FMulExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(f->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(f->right, width_out));
    assert(*width_out != 1 && "uncanonicalized FMul");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term3(tm, BITWUZLA_KIND_FP_MUL,
                          getRoundingModeTerm(f->roundingMode), left, right));
  }

  case Expr::FDiv: {
    FDivExpr *f = cast<FDivExpr>(e);
    BitwuzlaTermHandle left = castToFloat(construct(f->left, width_out));
    BitwuzlaTermHandle right = castToFloat(construct(f->right, width_out));
    assert(*width_out != 1 && "uncanonicalized FDiv");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term3(tm, BITWUZLA_KIND_FP_DIV,
                          getRoundingModeTerm(f->roundingMode), left, right));
  }

  case Expr::FSqrt: {
    FSqrtExpr *f = cast<FSqrtExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(f->expr, width_out));
    assert(*width_out != 1 && "uncanonicalized FSqrt");
    return BitwuzlaTermHandle(bitwuzla_mk_term2(
        tm, BITWUZLA_KIND_FP_SQRT, getRoundingModeTerm(f->roundingMode), arg));
  }

  case Expr::FAbs: {
    FAbsExpr *f = cast<FAbsExpr>(e);
    BitwuzlaTermHandle arg = castToFloat(construct(f->expr, width_out));
    assert(*width_out != 1 && "uncanonicalized FAbs");
    return BitwuzlaTermHandle(
        bitwuzla_mk_term1(tm, BITWUZLA_KIND_FP_ABS, arg));
  }

  default:
    assert(0 && "unhandled Expr type");
    return getTrue();
  }
}

#endif // ENABLE_BITWUZLA
