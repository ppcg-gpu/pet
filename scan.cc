/*
 * Copyright 2011      Leiden University. All rights reserved.
 * Copyright 2012-2015 Ecole Normale Superieure. All rights reserved.
 * Copyright 2015-2017 Sven Verdoolaege. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY LEIDEN UNIVERSITY ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LEIDEN UNIVERSITY OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Leiden University.
 */ 

#include "config.h"

#include <string.h>
#include <set>
#include <map>
#include <iostream>
#include <sstream>
#include <llvm/Support/raw_ostream.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTDiagnostic.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <isl/id.h>
#include <isl/space.h>
#include <isl/aff.h>
#include <isl/set.h>
#include <isl/union_set.h>

#include "aff.h"
#include "array.h"
#include "clang_compatibility.h"
#include "clang.h"
#include "context.h"
#include "expr.h"
#include "expr_plus.h"
#include "id.h"
#include "inliner.h"
#include "inlined_calls.h"
#include "killed_locals.h"
#include "nest.h"
#include "options.h"
#include "debug_hooks.h"
#include "scan.h"
#include "scop.h"
#include "scop_plus.h"
#include "substituter.h"
#include "tree.h"
#include "tree2scop.h"

using namespace std;
using namespace clang;

static enum pet_op_type UnaryOperatorKind2pet_op_type(UnaryOperatorKind kind)
{
	switch (kind) {
	case UO_Minus:
		return pet_op_minus;
	case UO_Not:
		return pet_op_not;
	case UO_LNot:
		return pet_op_lnot;
	case UO_PostInc:
		return pet_op_post_inc;
	case UO_PostDec:
		return pet_op_post_dec;
	case UO_PreInc:
		return pet_op_pre_inc;
	case UO_PreDec:
		return pet_op_pre_dec;
	default:
		return pet_op_last;
	}
}

static enum pet_op_type BinaryOperatorKind2pet_op_type(BinaryOperatorKind kind)
{
	switch (kind) {
	case BO_AddAssign:
		return pet_op_add_assign;
	case BO_SubAssign:
		return pet_op_sub_assign;
	case BO_MulAssign:
		return pet_op_mul_assign;
	case BO_DivAssign:
		return pet_op_div_assign;
	case BO_AndAssign:
		return pet_op_and_assign;
	case BO_XorAssign:
		return pet_op_xor_assign;
	case BO_OrAssign:
		return pet_op_or_assign;
	case BO_Assign:
		return pet_op_assign;
	case BO_Add:
		return pet_op_add;
	case BO_Sub:
		return pet_op_sub;
	case BO_Mul:
		return pet_op_mul;
	case BO_Div:
		return pet_op_div;
	case BO_Rem:
		return pet_op_mod;
	case BO_Shl:
		return pet_op_shl;
	case BO_Shr:
		return pet_op_shr;
	case BO_EQ:
		return pet_op_eq;
	case BO_NE:
		return pet_op_ne;
	case BO_LE:
		return pet_op_le;
	case BO_GE:
		return pet_op_ge;
	case BO_LT:
		return pet_op_lt;
	case BO_GT:
		return pet_op_gt;
	case BO_And:
		return pet_op_and;
	case BO_Xor:
		return pet_op_xor;
	case BO_Or:
		return pet_op_or;
	case BO_LAnd:
		return pet_op_land;
	case BO_LOr:
		return pet_op_lor;
	default:
		return pet_op_last;
	}
}

/* Is the size of "type" a thing the compiler can be asked about?
 *
 * Asking for the size of a type that is not complete means asking for
 * the layout of a record whose fields are not known, which is a fatal
 * error rather than a question with an empty answer.  A type declared
 * and not defined, and one that is still written in terms of a template
 * parameter, are both of that kind.
 */
static bool has_known_size(QualType type)
{
	if (type.isNull())
		return false;

	return !type->isIncompleteType() && !type->isDependentType() &&
		!type->isUndeducedType();
}

#ifdef GETTYPEINFORETURNSTYPEINFO

static int size_in_bytes(ASTContext &context, QualType type)
{
	if (!has_known_size(type))
		return 0;

	return context.getTypeInfo(type).Width / 8;
}

#else

static int size_in_bytes(ASTContext &context, QualType type)
{
	if (!has_known_size(type))
		return 0;

	return context.getTypeInfo(type).first / 8;
}

#endif

/* Check if the element type corresponding to the given array type
 * has a const qualifier.
 */
static bool const_base(QualType qt)
{
	const Type *type = qt.getTypePtr();

	if (type->isPointerType())
		return const_base(type->getPointeeType());
	if (type->isArrayType()) {
		const ArrayType *atype;
		type = type->getCanonicalTypeInternal().getTypePtr();
		atype = cast<ArrayType>(type);
		return const_base(atype->getElementType());
	}

	return qt.isConstQualified();
}

PetScan::~PetScan()
{
	std::map<const Type *, pet_expr *>::iterator it;
	std::map<FunctionDecl *, pet_function_summary *>::iterator it_s;

	for (it = type_size.begin(); it != type_size.end(); ++it)
		pet_expr_free(it->second);
	pet_function_summary_free(no_summary);
	for (it_s = summary_cache.begin(); it_s != summary_cache.end(); ++it_s)
		pet_function_summary_free(it_s->second);

	isl_id_to_pet_expr_free(id_size);
	isl_union_map_free(value_bounds);

	fprintf(stderr, "pet_walk guard counters: noreturn=%d depth=%d stmts=%d cycle=%d\n",
		own_walk.n_no_return, own_walk.n_depth, own_walk.n_too_many_stmts, own_walk.n_cycle);
}

/* Remember where a scop stopped and on what.
 *
 * Under autodetect nothing is reported, because a construct a scop
 * cannot hold is not a fault of the source: the scop simply ends there.
 * But where it ends, and on what, is the only thing that says why a
 * function of five hundred lines came out as nine statements, and
 * throwing it away is why nobody could say.  So it is written out --
 * one line for each place a scop stopped, saying where and what --
 * whenever PET_SCOP_TRACE is set in the environment.
 */
void PetScan::stopped_at(SourceRange range, const std::string &why)
{
	SourceLocation loc = range.getBegin();
	std::string where = loc.printToString(PP.getSourceManager());

	/* The first of them is kept, because that is the one that ended
	 * the scop; the rest are what was left over after it had ended.
	 */
	if (first_stop.empty())
		first_stop = where + ": " + why;
	last_stop = where + ": " + why;

	if (getenv("PET_SCOP_TRACE"))
		fprintf(stderr, "a scop stops at %s: %s\n",
			where.c_str(), why.c_str());
}

/* Report a diagnostic on the range "range", unless autodetect is set.
 */
void PetScan::report(SourceRange range, unsigned id)
{
	if (options->autodetect) {
		DiagnosticsEngine &diag = PP.getDiagnostics();

		stopped_at(range,
			diag.getDiagnosticIDs()->getDescription(id).str());
		return;
	}

	SourceLocation loc = range.getBegin();
	DiagnosticsEngine &diag = PP.getDiagnostics();
	DiagnosticBuilder B = diag.Report(loc, id) << range;
}

/* Report a diagnostic on "stmt", unless autodetect is set.
 */
void PetScan::report(Stmt *stmt, unsigned id)
{
	report(stmt->getSourceRange(), id);
}

/* Report a diagnostic on "decl", unless autodetect is set.
 */
void PetScan::report(Decl *decl, unsigned id)
{
	report(decl->getSourceRange(), id);
}

/* What kind of thing "stmt" is, in the words a reader would use.
 *
 * Everything a scan does not handle used to be reported as
 * "unsupported", one word for every construct there is, and over an
 * engine that one word was half of all the places a scop ended.  A map
 * of where scops end is only worth reading if each place says what it
 * met, so this names the construct.
 *
 * The names are of kinds, not of instances: a call says that it is a
 * call through a pointer or a call to a builtin, never which function,
 * or the map would have as many classes as the program has names.
 * Where there is nothing more to say than what clang calls the node,
 * that is what is said, which is still a name and not a shrug.
 */
static std::string what_it_is(Stmt *stmt)
{
	CallExpr *call;
	MemberExpr *member;

	if (!stmt)
		return "nothing at all";

	call = dyn_cast<CallExpr>(stmt);
	if (call) {
		FunctionDecl *fd = pet_clang_direct_callee(call);
		const FunctionDecl *def;

		if (!fd) {
			if (getenv("PET_CALLEE_TRACE")) {
				Expr *c = call->getCallee();

				fprintf(stderr, "the callee is");
				while (c) {
					fprintf(stderr, " %s",
						c->getStmtClassName());
					if (isa<ImplicitCastExpr>(c))
						c = cast<ImplicitCastExpr>(c)->
							getSubExpr();
					else if (isa<ParenExpr>(c))
						c = cast<ParenExpr>(c)->
							getSubExpr();
					else if (isa<DeclRefExpr>(c)) {
						NamedDecl *d = cast<DeclRefExpr>(c)->getDecl();

						fprintf(stderr, " -> %s, a %s",
							d->getNameAsString().c_str(),
							d->getDeclKindName());
						c = NULL;
					} else {
						c = NULL;
					}
				}
				fprintf(stderr, "\n");
			}
			return "a call through a pointer";
		}
		if (fd->getBuiltinID() != 0)
			return "a call to a builtin";
		if (fd->isVariadic())
			return "a call taking a variable number of arguments";
		if (!fd->hasBody(def))
			return "a call to a body the link does not hold";
		return "a call";
	}

	member = dyn_cast<MemberExpr>(stmt);
	if (member) {
		ValueDecl *d = member->getMemberDecl();
		RecordDecl *rd = d ? dyn_cast<RecordDecl>(d->getDeclContext())
				   : NULL;

		if (rd && rd->isUnion())
			return "a member of a union";
		return "a member of a record";
	}

	if (isa<CompoundLiteralExpr>(stmt))
		return "a compound literal";
	if (isa<StringLiteral>(stmt))
		return "a string written in the source";
	if (isa<InitListExpr>(stmt))
		return "a list of initial values";
	if (isa<UnaryExprOrTypeTraitExpr>(stmt))
		return "a question about a type";
	if (isa<ArraySubscriptExpr>(stmt))
		return "a subscript";
	if (isa<CastExpr>(stmt))
		return "a cast";
	if (isa<DeclRefExpr>(stmt))
		return "a name";

	return std::string("a ") + stmt->getStmtClassName();
}

/* Called if we found something we (currently) cannot handle.
 *
 * We only actually complain if autodetect is false.
 */
void PetScan::unsupported(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	std::string why = "unsupported: " + what_it_is(stmt);
	SourceRange range = stmt ? stmt->getSourceRange() : SourceRange();

	if (options->autodetect) {
		stopped_at(range, why);
		return;
	}

	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning, "%0");
	diag.Report(range.getBegin(), id) << why << range;
}

/* Report an unsupported unary operator, unless autodetect is set.
 */
void PetScan::report_unsupported_unary_operator(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
			       "this type of unary operator is not supported");
	report(stmt, id);
}

/* Report a union whose members are not all the same size, unless autodetect
 * is set.
 *
 * One member stands for the shared storage of all of them, which is exact
 * only while they index the same bytes.  Members of different sizes need the
 * index scaled by the element size, and until that exists this refuses
 * rather than computing with the wrong extent.
 */
void PetScan::report_unsupported_union_member_size(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
			       "a union whose members differ in size is not "
			       "supported: the members share storage and the "
			       "index would have to be scaled by the element "
			       "size");
	report(stmt, id);
}

/* THE THREE WAYS A STORAGE ANNOTATION CANNOT BE HONOURED.
 *
 * Each names the array and both numbers, because the whole point of the
 * annotation is that a human wrote it and a human has to be able to correct
 * it.  Each returns to a caller that refuses the expression, so the refusal
 * reaches the exit status: a guard that reports and carries on is worse than
 * no guard, which the union path demonstrated with four warnings, exit 0 and
 * a wrong answer.
 */
void PetScan::report_arena(Stmt *stmt, const std::string &why)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	std::string msg = "storage annotation: " + why;
	SourceRange range = stmt ? stmt->getSourceRange() : SourceRange();

	if (options->autodetect) {
		stopped_at(range, msg);
		return;
	}

	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning, "%0");
	diag.Report(range.getBegin(), id) << msg << range;
}

/* Report an unsupported binary operator, unless autodetect is set.
 */
void PetScan::report_unsupported_binary_operator(Stmt *stmt){
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
			       "this type of binary operator is not supported");
	report(stmt, id);
}

/* Report an unsupported statement type, unless autodetect is set.
 */
void PetScan::report_unsupported_statement_type(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
				   "this type of statement is not supported");
	report(stmt, id);
}

/* Report a missing prototype, unless autodetect is set.
 */
void PetScan::report_prototype_required(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "prototype required");
	report(stmt, id);
}

/* Report a missing increment, unless autodetect is set.
 */
void PetScan::report_missing_increment(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "missing increment");
	report(stmt, id);
}

/* Report a missing summary function, unless autodetect is set.
 */
void PetScan::report_missing_summary_function(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "missing summary function");
	report(stmt, id);
}

/* Report a missing summary function body, unless autodetect is set.
 */
void PetScan::report_missing_summary_function_body(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "missing summary function body");
	report(stmt, id);
}

/* Report an unsupported argument in a call to an inlined function,
 * unless autodetect is set.
 */
void PetScan::report_unsupported_inline_function_argument(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
				   "unsupported inline function call argument");
	report(stmt, id);
}

/* Report an unsupported type of declaration, unless autodetect is set.
 */
void PetScan::report_unsupported_declaration(Decl *decl)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
				   "unsupported declaration");
	report(decl, id);
}

/* Report an unbalanced pair of scop/endscop pragmas, unless autodetect is set.
 */
void PetScan::report_unbalanced_pragmas(SourceLocation scop,
	SourceLocation endscop)
{
	if (options->autodetect)
		return;

	DiagnosticsEngine &diag = PP.getDiagnostics();
	{
		unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "unbalanced endscop pragma");
		DiagnosticBuilder B2 = diag.Report(endscop, id);
	}
	{
		unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Note,
					   "corresponding scop pragma");
		DiagnosticBuilder B = diag.Report(scop, id);
	}
}

/* Report a return statement in an unsupported context,
 * unless autodetect is set.
 */
void PetScan::report_unsupported_return(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
			   "return statements not supported in this context");
	report(stmt, id);
}

/* Report a return statement that does not appear at the end of a function,
 * unless autodetect is set.
 */
void PetScan::report_return_not_at_end_of_function(Stmt *stmt)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
		       "return statement must be final statement in function");
	report(stmt, id);
}

/* Report the declaration of a variable with a recursive type.
 */
void PetScan::report_unsupported_recursive_type(Decl *decl)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
				   "unsupported recursive type variable");
	report(decl, id);
}

/* Extract an integer from "val", which is assumed to be non-negative.
 */
static __isl_give isl_val *extract_unsigned(isl_ctx *ctx,
	const llvm::APInt &val)
{
	unsigned n;
	const uint64_t *data;

	data = val.getRawData();
	n = val.getNumWords();
	return isl_val_int_from_chunks(ctx, n, sizeof(uint64_t), data);
}

/* Extract an integer from "val".  If "is_signed" is set, then "val"
 * is signed.  Otherwise it it unsigned.
 */
static __isl_give isl_val *extract_int(isl_ctx *ctx, bool is_signed,
	llvm::APInt val)
{
	int is_negative = is_signed && val.isNegative();
	isl_val *v;

	if (is_negative)
		val = -val;

	v = extract_unsigned(ctx, val);

	if (is_negative)
		v = isl_val_neg(v);
	return v;
}

/* Extract an integer from "expr".
 */
__isl_give isl_val *PetScan::extract_int(isl_ctx *ctx, IntegerLiteral *expr)
{
	const Type *type = expr->getType().getTypePtr();
	bool is_signed = type->hasSignedIntegerRepresentation();

	return ::extract_int(ctx, is_signed, expr->getValue());
}

/* Extract an integer from "expr".
 * Return NULL if "expr" does not (obviously) represent an integer.
 */
__isl_give isl_val *PetScan::extract_int(clang::ParenExpr *expr)
{
	return extract_int(expr->getSubExpr());
}

/* Extract an integer from "expr".
 * Return NULL if "expr" does not (obviously) represent an integer.
 */
__isl_give isl_val *PetScan::extract_int(clang::Expr *expr)
{
	if (expr->getStmtClass() == Stmt::IntegerLiteralClass)
		return extract_int(ctx, cast<IntegerLiteral>(expr));
	if (expr->getStmtClass() == Stmt::ParenExprClass)
		return extract_int(cast<ParenExpr>(expr));

	unsupported(expr);
	return NULL;
}

/* Extract a pet_expr from the APInt "val", which is assumed
 * to be non-negative.
 */
__isl_give pet_expr *PetScan::extract_expr(const llvm::APInt &val)
{
	return pet_expr_new_int(extract_unsigned(ctx, val));
}

/* Return the number of bits needed to represent the type of "decl",
 * if it is an integer type.  Otherwise return 0.
 * If qt is signed then return the opposite of the number of bits.
 */
static int get_type_size(ValueDecl *decl)
{
	return pet_clang_get_type_size(decl->getType(), decl->getASTContext());
}

/* Bound parameter "pos" of "set" to the possible values of "decl".
 */
static __isl_give isl_set *set_parameter_bounds(__isl_take isl_set *set,
	unsigned pos, ValueDecl *decl)
{
	int type_size;
	isl_ctx *ctx;
	isl_val *bound;

	ctx = isl_set_get_ctx(set);
	type_size = get_type_size(decl);
	if (type_size == 0)
		isl_die(ctx, isl_error_invalid, "not an integer type",
			return isl_set_free(set));
	if (type_size > 0) {
		set = isl_set_lower_bound_si(set, isl_dim_param, pos, 0);
		bound = isl_val_int_from_ui(ctx, type_size);
		bound = isl_val_2exp(bound);
		bound = isl_val_sub_ui(bound, 1);
		set = isl_set_upper_bound_val(set, isl_dim_param, pos, bound);
	} else {
		bound = isl_val_int_from_ui(ctx, -type_size - 1);
		bound = isl_val_2exp(bound);
		bound = isl_val_sub_ui(bound, 1);
		set = isl_set_upper_bound_val(set, isl_dim_param, pos,
						isl_val_copy(bound));
		bound = isl_val_neg(bound);
		bound = isl_val_sub_ui(bound, 1);
		set = isl_set_lower_bound_val(set, isl_dim_param, pos, bound);
	}

	return set;
}

__isl_give pet_expr *PetScan::extract_index_expr(ImplicitCastExpr *expr)
{
	return extract_index_expr(expr->getSubExpr());
}

/* Construct a pet_expr representing an index expression for an access
 * to the variable referenced by "expr".
 *
 * If "expr" references an enum constant, then return an integer expression
 * instead, representing the value of the enum constant.
 */
__isl_give pet_expr *PetScan::extract_index_expr(DeclRefExpr *expr)
{
	return extract_index_expr(expr->getDecl());
}

/* Construct a pet_expr representing an index expression for an access
 * to the variable "decl".
 *
 * If "decl" is an enum constant, then we return an integer expression
 * instead, representing the value of the enum constant.
 *
 * An access to a variable with a recursive type is not allowed.
 */
__isl_give pet_expr *PetScan::extract_index_expr(ValueDecl *decl)
{
	isl_id *id;

	if (isa<EnumConstantDecl>(decl))
		return extract_expr(cast<EnumConstantDecl>(decl));

	id = pet_id_from_decl(ctx, decl);
	if (is_recursive(pet_id_get_array_type(id))) {
		report_unsupported_recursive_type(decl);
		isl_id_free(id);
		return NULL;
	}
	return pet_id_create_index_expr(id);
}

/* Construct a pet_expr representing the index expression "expr"
 * Return NULL on error.
 *
 * If "expr" is a reference to an enum constant, then return
 * an integer expression instead, representing the value of the enum constant.
 */
/* The object a method was called on, as something that can be named.
 *
 * A method reaches its members through "this", which the source does
 * not have to write and which clang gives no declaration: there is a
 * CXXThisExpr and nothing behind it.  Everything else a scop reads
 * through is a declaration -- a parameter, a local -- and every path
 * that works out which element is meant starts from one.
 *
 * So one is made, once for each method, and "this" is read through it
 * exactly as "p" is read through in "p->field".  Made once and kept,
 * because the body of a method is extracted by one scan and the object
 * it was called on is bound by another, and the two have to be talking
 * about the same thing.
 */
static ImplicitParamDecl *implicit_object(ASTContext &ast_context,
	CXXMethodDecl *method)
{
	static std::map<CXXMethodDecl *, ImplicitParamDecl *> known;
	std::map<CXXMethodDecl *, ImplicitParamDecl *>::iterator it;

	it = known.find(method);
	if (it != known.end())
		return it->second;

	IdentifierInfo *name = &ast_context.Idents.get("__pet_this");
	ImplicitParamDecl *decl = ImplicitParamDecl::Create(ast_context,
		method, method->getLocation(), name, method->getThisType(),
		ImplicitParamKind::CXXThis);

	known[method] = decl;

	return decl;
}

/* Extract an index expression from "expr".
 *
 * Parentheses and the wrapper clang puts around an expression whose
 * value it has worked out are not things in themselves: what is written
 * inside them is what is being subscripted or reached into, and the
 * wrapper only says something about how it was read.  A base written
 * with brackets around it used to end the scop for that reason alone.
 */
__isl_give pet_expr *PetScan::extract_index_expr(Expr *expr)
{
	switch (expr->getStmtClass()) {
	case Stmt::ImplicitCastExprClass:
		return extract_index_expr(cast<ImplicitCastExpr>(expr));
	case Stmt::DeclRefExprClass:
		return extract_index_expr(cast<DeclRefExpr>(expr));
	case Stmt::ArraySubscriptExprClass:
		return extract_index_expr(cast<ArraySubscriptExpr>(expr));
	case Stmt::IntegerLiteralClass:
		return extract_expr(cast<IntegerLiteral>(expr));
	case Stmt::MemberExprClass:
		return extract_index_expr(cast<MemberExpr>(expr));
	case Stmt::ParenExprClass:
		return extract_index_expr(cast<ParenExpr>(expr)->getSubExpr());
	case Stmt::ConstantExprClass:
		return extract_index_expr(
			cast<ConstantExpr>(expr)->getSubExpr());
	case Stmt::CXXThisExprClass: {
		CXXMethodDecl *method;

		method = dyn_cast_or_null<CXXMethodDecl>(decl_context);
		if (method)
			return extract_index_expr(
				implicit_object(ast_context, method));
		break;
	}
	default:
		break;
	}

	unsupported(expr);
	return NULL;
}

/* Extract an index expression from the given array subscript expression.
 *
 * We first extract an index expression from the base.
 * This will result in an index expression with a range that corresponds
 * to the earlier indices.
 * We then extract the current index and let
 * pet_expr_access_subscript combine the two.
 */
__isl_give pet_expr *PetScan::extract_index_expr(ArraySubscriptExpr *expr)
{
	Expr *base = expr->getBase();
	Expr *idx = expr->getIdx();
	pet_expr *index;
	pet_expr *base_expr;

	base_expr = extract_index_expr(base);
	index = extract_expr(idx);

	base_expr = pet_expr_access_subscript(base_expr, index);

	return base_expr;
}

/* Extract an index expression from a member expression.
 *
 * If the base access (to the structure containing the member)
 * is of the form
 *
 *	A[..]
 *
 * and the member is called "f", then the member access is of
 * the form
 *
 *	A_f[A[..] -> f[]]
 *
 * If the member access is to an anonymous struct, then simply return
 *
 *	A[..]
 *
 * If the member access in the source code is of the form
 *
 *	A->f
 *
 * then it is treated as
 *
 *	A[0].f
 */
__isl_give pet_expr *PetScan::extract_index_expr(MemberExpr *expr)
{
	Expr *base = expr->getBase();
	FieldDecl *field = cast<FieldDecl>(expr->getMemberDecl());
	pet_expr *base_index;
	isl_id *id;

	base_index = extract_index_expr(base);

	if (expr->isArrow()) {
		pet_expr *index = pet_expr_new_int(isl_val_zero(ctx));
		base_index = pet_expr_access_subscript(base_index, index);
	}

	if (field->isAnonymousStructOrUnion())
		return base_index;

	id = pet_id_from_decl(ctx, field);

	return pet_expr_access_member(base_index, id);
}

/* Mark the given access pet_expr as a write.
 */
static __isl_give pet_expr *mark_write(__isl_take pet_expr *access)
{
	access = pet_expr_access_set_write(access, 1);
	access = pet_expr_access_set_read(access, 0);

	return access;
}

/* Mark the given (read) access pet_expr as also possibly being written.
 * That is, initialize the may write access relation from the may read relation
 * and initialize the must write access relation to the empty relation.
 */
static __isl_give pet_expr *mark_may_write(__isl_take pet_expr *expr)
{
	isl_union_map *access;
	isl_union_map *empty;

	access = pet_expr_access_get_dependent_access(expr,
						pet_expr_access_may_read);
	empty = isl_union_map_empty(isl_union_map_get_space(access));
	expr = pet_expr_access_set_access(expr, pet_expr_access_may_write,
					    access);
	expr = pet_expr_access_set_access(expr, pet_expr_access_must_write,
					    empty);

	return expr;
}

/* Construct a pet_expr representing a unary operator expression.
 */
__isl_give pet_expr *PetScan::extract_expr(UnaryOperator *expr)
{
	int type_size;
	pet_expr *arg;
	enum pet_op_type op;

	op = UnaryOperatorKind2pet_op_type(expr->getOpcode());
	if (op == pet_op_last) {
		report_unsupported_unary_operator(expr);
		return NULL;
	}

	arg = extract_expr(expr->getSubExpr());

	if (expr->isIncrementDecrementOp() &&
	    pet_expr_get_type(arg) == pet_expr_access) {
		arg = mark_write(arg);
		arg = pet_expr_access_set_read(arg, 1);
	}

	type_size = pet_clang_get_type_size(expr->getType(), ast_context);
	return pet_expr_new_unary(type_size, op, arg);
}

/* Construct a pet_expr representing a binary operator expression.
 *
 * If the top level operator is an assignment and the LHS is an access,
 * then we mark that access as a write.  If the operator is a compound
 * assignment, the access is marked as both a read and a write.
 */
__isl_give pet_expr *PetScan::extract_expr(BinaryOperator *expr)
{
	int type_size;
	pet_expr *lhs, *rhs;
	enum pet_op_type op;

	op = BinaryOperatorKind2pet_op_type(expr->getOpcode());
	if (op == pet_op_last) {
		report_unsupported_binary_operator(expr);
		return NULL;
	}

	lhs = extract_expr(expr->getLHS());
	rhs = extract_expr(expr->getRHS());

	if (expr->isAssignmentOp() &&
	    pet_expr_get_type(lhs) == pet_expr_access) {
		lhs = mark_write(lhs);
		if (expr->isCompoundAssignmentOp())
			lhs = pet_expr_access_set_read(lhs, 1);
	}

	type_size = pet_clang_get_type_size(expr->getType(), ast_context);
	return pet_expr_new_binary(type_size, op, lhs, rhs);
}

/* Construct a pet_tree for a variable declaration and
 * add the declaration to the list of declarations
 * inside the current compound statement.
 */
__isl_give pet_tree *PetScan::extract(Decl *decl)
{
	VarDecl *vd;
	pet_expr *lhs, *rhs;
	pet_tree *tree;

	/* A TYPE DECLARED INSIDE THE BLOCK IS NOT A STATEMENT.  `union { float
	 * f; uint32_t u; } c;` is two declarations to clang -- the record and
	 * the variable -- and stopping on the record refused the whole scop
	 * over a construct that allocates nothing and computes nothing.  The
	 * ALIASING the union states is not lost by skipping it: it is carried
	 * by the member accesses, which extract_index_expr maps onto one
	 * storage.  Skipping the type without that would be the silent half
	 * of the fix, and worse than refusing.
	 */
	if (isa<RecordDecl>(decl) || isa<TypedefNameDecl>(decl) ||
	    isa<EnumDecl>(decl))
		return pet_tree_new_block(ctx, 0, 0);

	if (!isa<VarDecl>(decl)) {
		report_unsupported_declaration(decl);
		return NULL;
	}

	vd = cast<VarDecl>(decl);
	declarations.push_back(vd);

	lhs = extract_access_expr(vd);
	lhs = mark_write(lhs);
	if (!vd->getInit())
		return pet_tree_new_decl(lhs);

	/* A CALL IN AN INITIALISER IS A CALL.
	 *
	 * Bodies are put in place by pet_inlined_calls, and it was reached
	 * from one place only: PetScan::extract_expr_stmt, an expression
	 * statement.  An initialiser is not one, so
	 *
	 *	y[i] = f(x[i]);		the body of f comes over
	 *	const float d = f(x[i]);	f stays a call
	 *
	 * and which of the two a program is written in decides whether its
	 * arithmetic can be read.  Measured on ggml's q8_0 row pair: inside
	 * one inlined body, `base = fp32_from_bits(...) + base;` came over
	 * and `const uint32_t w = fp32_to_bits(f);` two lines above it did
	 * not, so the same union pun was transparent in one line and opaque
	 * in the next.  ggml declares nearly every intermediate const, so
	 * this reached the whole engine: the f16 conversion of a
	 * dequantiser, `const float d = GGML_FP16_TO_FP32(x[i].d);`, was a
	 * call in a scop that was otherwise fully scheduled.
	 *
	 * THE DECLARATION STAYS OUTSIDE THE BLOCK THE BODIES GO IN.
	 * pet_inlined_calls::add_inlined wraps its result in a block that is
	 * a scope, which is right for the return variables it declares there
	 * and wrong for this one: a variable declared inside a scope is
	 * killed at its end, so `d` was dead before the loop that reads it,
	 * the read depended on nothing, and isl was free to schedule the
	 * whole conversion AFTER the multiplication that wanted it.  That is
	 * what came out -- measured on the dequantiser, y[] written from an
	 * undefined d and the conversion trailing behind it.
	 *
	 * So the declaration is emitted on its own, the initialiser becomes
	 * an assignment to it, and only the assignment goes inside the block
	 * with the bodies.  The two are grouped by a block that is NOT a
	 * scope (the 0), which is the same thing extract(DeclStmt) does for
	 * a declaration of several variables.
	 */
	{
		pet_inlined_calls ic(this);
		pet_expr *var;
		pet_tree *body, *group;
		int type_size;

		ic.collect(vd->getInit());
		call2id = &ic.call2id;
		rhs = extract_expr(vd->getInit());
		call2id = NULL;

		if (ic.inlined.empty())
			return pet_tree_new_decl_init(lhs, rhs);

		var = mark_write(extract_access_expr(vd));
		type_size = pet_clang_get_type_size(vd->getType(), ast_context);
		body = pet_tree_new_expr(pet_expr_new_binary(type_size,
						pet_op_assign, var, rhs));
		body = ic.add_inlined(body);

		group = pet_tree_new_block(ctx, 0, 2);
		group = pet_tree_block_add_child(group, pet_tree_new_decl(lhs));
		group = pet_tree_block_add_child(group, body);

		return group;
	}
}

/* Construct a pet_tree for a variable declaration statement.
 * If the declaration statement declares multiple variables,
 * then return a group of pet_trees, one for each declared variable.
 */
__isl_give pet_tree *PetScan::extract(DeclStmt *stmt)
{
	pet_tree *tree;
	unsigned n;

	if (!stmt->isSingleDecl()) {
		const DeclGroup &group = stmt->getDeclGroup().getDeclGroup();
		n = group.size();
		tree = pet_tree_new_block(ctx, 0, n);

		for (unsigned i = 0; i < n; ++i) {
			pet_tree *tree_i;
			pet_loc *loc;

			tree_i = extract(group[i]);
			loc = construct_pet_loc(group[i]->getSourceRange(),
						false);
			tree_i = pet_tree_set_loc(tree_i, loc);
			tree = pet_tree_block_add_child(tree, tree_i);
		}

		return tree;
	}

	return extract(stmt->getSingleDecl());
}

/* Construct a pet_expr representing a conditional operation.
 */
__isl_give pet_expr *PetScan::extract_expr(ConditionalOperator *expr)
{
	pet_expr *cond, *lhs, *rhs;

	cond = extract_expr(expr->getCond());
	lhs = extract_expr(expr->getTrueExpr());
	rhs = extract_expr(expr->getFalseExpr());

	return pet_expr_new_ternary(cond, lhs, rhs);
}

__isl_give pet_expr *PetScan::extract_expr(ImplicitCastExpr *expr)
{
	return extract_expr(expr->getSubExpr());
}

/* Construct a pet_expr representing a floating point value.
 *
 * If the floating point literal does not appear in a macro,
 * then we use the original representation in the source code
 * as the string representation.  Otherwise, we use the pretty
 * printer to produce a string representation.
 */
__isl_give pet_expr *PetScan::extract_expr(FloatingLiteral *expr)
{
	double d;
	string s;
	const LangOptions &LO = PP.getLangOpts();
	SourceLocation loc = expr->getLocation();

	if (!loc.isMacroID()) {
		SourceManager &SM = PP.getSourceManager();
		unsigned len = Lexer::MeasureTokenLength(loc, SM, LO);
		s = string(SM.getCharacterData(loc), len);
	} else {
		llvm::raw_string_ostream S(s);
		expr->printPretty(S, 0, PrintingPolicy(LO));
		S.str();
	}
	d = expr->getValueAsApproximateDouble();
	return pet_expr_new_double(ctx, d, s.c_str());
}

/* Extract an index expression from "expr" and then convert it into
 * an access pet_expr.
 *
 * If "expr" is a reference to an enum constant, then return
 * an integer expression instead, representing the value of the enum constant.
 */
/* Is "expr" a reference to a member of a union?
 *
 * The members of a union are one storage read and written under different
 * types.  A dependence analysis told otherwise is free to exchange a write
 * through one member with a read through another, and the read then takes
 * bits that were never written -- measured, and kept as
 * tests/pet-union/decl_only for the record.
 */
/* The size in BYTES of what one subscript of "qt" steps over.
 *
 * For a scalar member that is the member; for an array member it is the
 * array's element, because that is what an index counts.  Not
 * pet_clang_get_type_size: that returns 0 for anything which is not an
 * integer and a SIGNED BIT WIDTH for anything which is, so it called float
 * and int32_t different sizes and refused every union in the tree.  Caught
 * because the refusal fired on the reproducers that were supposed to go
 * green.
 */
static uint64_t element_size(QualType qt, ASTContext &ast_context)
{
	QualType elem = ast_context.getBaseElementType(qt);

	return ast_context.getTypeSizeInChars(elem).getQuantity();
}

/* The member of "record" whose element is smallest, or NULL if there is none.
 *
 * Smallest rather than first: see union_member_storage below for why that
 * turns two constructions into one.
 */
static FieldDecl *union_canonical_field(RecordDecl *record,
	ASTContext &ast_context)
{
	FieldDecl *canon = NULL;
	uint64_t best = 0;

	for (auto *f : record->fields()) {
		uint64_t size = element_size(f->getType(), ast_context);
		if (!size)
			return NULL;
		if (!canon || size < best) {
			canon = f;
			best = size;
		}
	}

	return canon;
}

/* Put the subscripts of "expr" back on "index", outermost last, with the
 * innermost one scaled.
 *
 * "expr" is a chain of ArraySubscriptExpr ending in the union member, and
 * "index" is that member replaced by the canonical one.  The subscripts are
 * what carries a window's OFFSET -- "g->i[1024]" is the buffer 4096 bytes
 * into the storage -- so dropping them would collapse every window in the
 * group onto the first, which is a wrong answer rather than a lost one.
 *
 * When the member's element is "scale" canonical elements wide, the last
 * subscript "e" becomes "scale * e + shift"; the caller asks for every shift
 * in 0 .. scale-1 and unions the relations, which is how one access lands on
 * the whole range of canonical elements it really touches.  With scale 1 the
 * expression is left exactly as it was rather than multiplied by one, so the
 * common case produces the same index it always did.
 */
__isl_give pet_expr *PetScan::carry_subscripts(Expr *expr,
	__isl_take pet_expr *index, int scale, int shift)
{
	ArraySubscriptExpr *sub = dyn_cast<ArraySubscriptExpr>(expr);
	pet_expr *idx;

	if (!sub)
		return index;

	index = carry_subscripts(sub->getBase()->IgnoreParenImpCasts(), index,
				 1, 0);
	idx = extract_expr(sub->getIdx());
	if (scale != 1) {
		idx = pet_expr_new_binary(0, pet_op_mul, idx,
			pet_expr_new_int(isl_val_int_from_si(ctx, scale)));
		if (shift)
			idx = pet_expr_new_binary(0, pet_op_add, idx,
			    pet_expr_new_int(isl_val_int_from_si(ctx, shift)));
	}

	return pet_expr_access_subscript(index, idx);
}

/* The union member access at the root of "expr", looking through subscripts,
 * or NULL if there is none.
 *
 * A union member that is an ARRAY is not reached as a MemberExpr: "g->f[i]"
 * is an ArraySubscriptExpr whose base is the member.  Testing "expr" itself
 * therefore missed every array member, and those are the interesting ones --
 * a union of arrays is how one storage holding many buffers at many offsets
 * is described, with each buffer a window addressed by an offset in the
 * index.  Measured before this: two float loops either side of a read
 * through a different member of the same union were fused and the read moved
 * past them, because the members were two independent arrays.
 */
static MemberExpr *union_member_root(Expr *expr)
{
	MemberExpr *member;
	FieldDecl *field;

	while (ArraySubscriptExpr *sub = dyn_cast<ArraySubscriptExpr>(expr))
		expr = sub->getBase()->IgnoreParenImpCasts();

	member = dyn_cast<MemberExpr>(expr);
	if (!member)
		return NULL;
	field = dyn_cast<FieldDecl>(member->getMemberDecl());
	if (!field || !field->getParent() || !field->getParent()->isUnion())
		return NULL;

	return member;
}

/* Is "expr" a reference to a member of a union?
 *
 * The members of a union are one storage read and written under different
 * types.  A dependence analysis told otherwise is free to exchange a write
 * through one member with a read through another, and the read then takes
 * bits that were never written -- measured, and kept as
 * tests/pet-union/decl_only for the record.
 */
static FieldDecl *union_field(Expr *expr)
{
	MemberExpr *member = union_member_root(expr);

	if (!member)
		return NULL;

	return cast<FieldDecl>(member->getMemberDecl());
}

/* The storage a union member shares with its siblings, as an access.
 *
 * ONE MEMBER IS CHOSEN TO STAND FOR THE STORAGE.
 *
 * The obvious base is the union object itself, and that is what this did.
 * It works when the union is an object -- pet has an array for it -- and
 * fails when the union is reached through a pointer: the relation lands on
 * a space that names no array pet knows, so nothing is live-out through it
 * and ppcg reports "Eliminated dead instances: { S_2[] }" for a write to the
 * CALLER'S memory.  Measured with --verbose, which is how this was found;
 * the same kernel with `struct` in place of `union` keeps the statement.
 *
 * Naming one member instead makes the relation land on an array pet
 * declares in either case, and the members alias because they all name the
 * same one.  The arrow is followed the way extract_index_expr follows it, so
 * "b->f" and "(*b).f" would describe the same storage.
 *
 * SUBSCRIPTS ARE CARRIED ACROSS.  "g->i[k]" becomes the same subscript on
 * the canonical member, so a window at an offset stays at that offset: the
 * offset lives in the index, which is what lets one union describe buffers
 * that begin at different addresses and overlap each other.
 *
 * THE CANONICAL MEMBER IS THE ONE WITH THE SMALLEST ELEMENT, and that choice
 * is what makes the arithmetic one case instead of two.  With the FIRST
 * member standing for the storage, an access through a larger element covers
 * a RANGE of canonical elements while an access through a smaller one lies
 * INSIDE a canonical element at a floored index -- two constructions, one of
 * them quasi-affine, and a union of f16, f32 and i64 needs both at once.
 * Taking the smallest leaves only the range: every other element is a whole
 * number of canonical ones, "scale" of them, and the relation is the union
 * of "scale" ordinary accesses at s*i, s*i+1, ... s*i+scale-1.  Measured
 * both ways round in llama-dspark's tests/ppcg/arena-probe.py, which puts
 * the smaller member first in one form and last in the other; with this
 * choice the two forms are the same case and both come out right.
 *
 * A RANGE AND NOT ONE ELEMENT, because the footprint of a write must not be
 * understated.  An f32 store at index i touches canonical elements 2i and
 * 2i+1; naming only 2i leaves 2i+1 unclaimed, and an unclaimed byte is a
 * licence for exactly the reordering this exists to forbid.
 *
 * WHAT IS STILL REFUSED, with a message rather than a wrong answer: element
 * sizes that are not multiples of the smallest, a scaled access through a
 * member that is not an array (there is no subscript to scale), and a scaled
 * access through an array of more than one dimension (the inner strides
 * would have to be scaled too, and that is not this).
 */
__isl_give pet_expr *PetScan::union_member_storage(Expr *expr, int scale,
	int shift)
{
	MemberExpr *member = union_member_root(expr);
	FieldDecl *field = cast<FieldDecl>(member->getMemberDecl());
	FieldDecl *canon = union_canonical_field(field->getParent(),
						 ast_context);
	pet_expr *index;

	if (!canon)
		return NULL;

	index = extract_index_expr(member->getBase());
	if (member->isArrow())
		index = pet_expr_access_subscript(index,
					pet_expr_new_int(isl_val_zero(ctx)));
	index = pet_expr_access_member(index, pet_id_from_decl(ctx, canon));
	index = carry_subscripts(expr, index, scale, shift);

	return pet_expr_access_from_index(canon->getType(), index, ast_context);
}

/* How many canonical elements does one element of the member accessed by
 * "expr" cover?  0 if this union cannot be described, and the reason has
 * been reported.
 *
 * This is where every boundary of the union path is drawn, in one place, so
 * that a shape which cannot be modelled costs a diagnostic and a non-zero
 * exit rather than a plausible wrong answer.
 */
int PetScan::union_scale(Expr *expr)
{
	MemberExpr *member = union_member_root(expr);
	FieldDecl *field = cast<FieldDecl>(member->getMemberDecl());
	FieldDecl *canon = union_canonical_field(field->getParent(),
						 ast_context);
	uint64_t mine, unit;

	if (!canon)
		return 0;
	unit = element_size(canon->getType(), ast_context);
	mine = element_size(field->getType(), ast_context);
	if (!unit || !mine)
		return 0;
	if (mine == unit)
		return 1;

	/* An element that is not a whole number of canonical ones has no
	 * exact range at all: it would start inside one and end inside
	 * another.  Every type in reach here is a power of two wide, so this
	 * is a guard rather than a case.
	 */
	if (mine % unit) {
		report_unsupported_union_member_size(member);
		return 0;
	}
	/* A scaled access needs a subscript to scale.  A scalar member has
	 * none, so a union mixing a float with an int64_t cannot be carried
	 * this way even though the arithmetic would be fine.
	 */
	if (!dyn_cast<ArraySubscriptExpr>(expr)) {
		report_unsupported_union_member_size(member);
		return 0;
	}
	/* More than one dimension would need the inner strides scaled too. */
	if (dyn_cast<ArraySubscriptExpr>(
		cast<ArraySubscriptExpr>(expr)->getBase()
		    ->IgnoreParenImpCasts())) {
		report_unsupported_union_member_size(member);
		return 0;
	}

	return mine / unit;
}

/* Construct an access pet_expr from "expr", an access to a member of a union.
 *
 * The INDEX keeps the member, because the index is what is printed and
 * "c.f" is what the source says.  The ACCESS RELATION is built from the
 * BASE instead, because the relation is what the dependence analysis reads
 * and the base is the storage the members share.  pet keeps those two apart
 * already -- a relation derived from the index is only a default, taken when
 * none was given -- so this gives the one that was missing rather than
 * bending the other.
 *
 * The read and write flags are restored afterwards:
 * pet_expr_access_set_access marks the expression according to the type of
 * relation it is handed, and at this point nobody has yet said whether this
 * access is a read or a write.
 */
__isl_give pet_expr *PetScan::access_from_union_member(Expr *expr,
	__isl_take pet_expr *index)
{
	pet_expr *access, *base;
	isl_union_map *relation;
	isl_bool was_read, was_write;
	enum pet_expr_access_type type[] = { pet_expr_access_may_read,
					     pet_expr_access_may_write,
					     pet_expr_access_must_write };
	int i, scale, shift;

	access = pet_expr_access_from_index(expr->getType(), index,
					    ast_context);
	scale = union_scale(expr);
	relation = NULL;
	for (shift = 0; scale > 0 && shift < scale; ++shift) {
		isl_union_map *part;

		base = union_member_storage(expr, scale, shift);
		/* PET_DEBUG_UNION: THE EXPRESSIONS AND THE RELATION BETWEEN
		 * THEM.
		 *
		 * This function is where a union's members are made to share
		 * storage, and when it is wrong the symptom is a statement
		 * that is simply not in the scop.  Nothing about that says
		 * which member, which base, or what relation was installed,
		 * and the search for it was a gdb session over generated C.
		 * Under this variable the access, the storage it is pointed
		 * at, and the relation that joins them are all printed, and
		 * the difference that mattered -- an argument dimension
		 * present or projected away -- is visible in one run.
		 */
		if (getenv("PET_DEBUG_UNION")) {
			fprintf(stderr, "pet union member: the access\n");
			pet_expr_dump(access);
			fprintf(stderr, "pet union member: storage %d of %d\n",
				shift, scale);
			pet_expr_dump(base);
		}
		if (!base)
			break;
		/* THE DEPENDENT ACCESS, not the may-read.
		 *
		 * pet_expr_access_get_may_read projects out the argument
		 * dimensions, which costs nothing when the base has none --
		 * a union declared as an object -- and everything when it
		 * has one.  Through a pointer the base is b[i0], so the
		 * relation came back as
		 * "{ [] -> b_f[b[o0] -> f[]] : o0 >= 0 }": the whole array,
		 * from an empty domain, with the argument gone.  Installed
		 * as the must-write of "b->f = ...", that write pinned down
		 * no element, was not live out of the scop, and ppcg
		 * reported "Eliminated dead instances: { S_2[] }" for a
		 * store into the caller's memory.
		 *
		 * pet_expr_access_set_access wants a dependent access --
		 * mark_may_write above feeds it one -- so this asks for the
		 * same thing.
		 */
		part = pet_expr_access_get_dependent_access(base,
						pet_expr_access_may_read);
		pet_expr_free(base);
		if (!part)
			break;
		relation = relation ? isl_union_map_union(relation, part) : part;
	}
	/* A UNION THIS CANNOT DESCRIBE MUST NOT BE DESCRIBED WRONGLY.
	 *
	 * This returned the access without the shared relation, so a union
	 * union_member_storage had just REFUSED went on to be scheduled with
	 * its members as independent arrays -- which is the whole defect the
	 * union path exists to remove.  Measured: a union of uint16_t beside
	 * float printed four warnings, exited 0, and read 16384 out of an
	 * index whose value was 7 -- 0x4000, the low half of the 1026.0f that
	 * had been written over it.  A loud warning beside a wrong answer is
	 * the silent third outcome wearing a diagnostic.
	 */
	if (!access || scale <= 0 || shift != scale || !relation) {
		pet_expr_free(access);
		isl_union_map_free(relation);
		return NULL;
	}

	was_read = pet_expr_access_is_read(access);
	was_write = pet_expr_access_is_write(access);

	if (getenv("PET_DEBUG_UNION")) {
		fprintf(stderr, "pet union member: the relation it installs\n");
		isl_union_map_dump(relation);
	}

	for (i = 0; i < 3; ++i)
		access = pet_expr_access_set_access(access, type[i],
						isl_union_map_copy(relation));
	isl_union_map_free(relation);

	access = pet_expr_access_set_read(access, was_read);
	access = pet_expr_access_set_write(access, was_write);

	return access;
}

/* The size in BYTES of one element of an array-or-pointer declaration.
 *
 * NOT element_size: that stops at getBaseElementType, which strips array
 * types and leaves a POINTER alone, so "float * lo" measured 8 -- the size
 * of the pointer -- and every stride was then judged against it.  The union
 * path never sees a pointer here because its declarations are fields, which
 * is why the two are separate rather than one function bent to serve both.
 */
static uint64_t pointee_element_size(QualType qt, ASTContext &ast_context)
{
	if (qt->isPointerType())
		qt = qt->getPointeeType();

	return element_size(qt, ast_context);
}

/* WHICH SEPARATELY DECLARED ARRAYS ARE ONE PIECE OF STORAGE.
 *
 * Filled by the "#pragma ppcg arena" handler in pet.cc before the scan runs,
 * read here while access relations are built.  A file-scope map rather than
 * a constructor argument because PetScan is built in five places and none of
 * them has anything else to say about storage.
 *
 * The key is the ValueDecl, which is what the pragma resolves its names to
 * and what an access ultimately refers to, so the two ends agree without a
 * string comparison anywhere.
 */
struct pet_arena_entry {
	ValueDecl *rep;
	long offset;
};
static std::map<ValueDecl *, pet_arena_entry> pet_arena_map;

void pet_arena_clear(void)
{
	pet_arena_map.clear();
}

void pet_arena_add(ValueDecl *decl, ValueDecl *rep, long offset)
{
	struct pet_arena_entry e = { rep, offset };

	pet_arena_map[decl] = e;
}

/* The array "decl" shares storage with, and its byte offset within it,
 * or NULL if it shares storage with nothing.
 *
 * The representative shares storage with itself at offset 0, and answering
 * NULL for it leaves its accesses on the ordinary path: composing an array
 * into itself at offset 0 is the identity, and not doing it keeps the common
 * case exactly as it was.
 */
static ValueDecl *pet_arena_lookup(ValueDecl *decl, long *offset)
{
	std::map<ValueDecl *, pet_arena_entry>::iterator it;

	it = pet_arena_map.find(decl);
	if (it == pet_arena_map.end())
		return NULL;
	if (it->second.rep == decl)
		return NULL;
	*offset = it->second.offset;

	return it->second.rep;
}

/* The lowest element index of "rep" that the annotation composes onto it,
 * 0 if nothing reaches below its own start.  "unit" is the size of one of
 * its elements.
 *
 * THE EXTENT HAS TO COVER WHAT THE COMPOSITION LANDS ON.  pet gives every
 * array the natural universe, and scop_collect_accesses intersects each
 * access's range with it.  An annotation is allowed to put the
 * representative anywhere in the storage -- "arena h 0 lo -16384 idx
 * -12288" -- and then a member's composed access has a NEGATIVE index into
 * the representative, which "i0 >= 0" removes.  Silently, and from every
 * relation at once, so the access is not misplaced but absent: on the aneg
 * probe both the read of idx[0] and the write of lo[i] vanished, dep_false
 * came out empty, nothing forbade the fusion, and the read crossed the
 * write.  Measured -- before: "[S_2[i] -> __pet_ref_4[]] -> h[o0] :
 * o0 = -8192 + 2i or o0 = -8191 + 2i", extents: "h[i0] : i0 >= 0",
 * after: the S_2 entry is gone.
 *
 * A member whose offset is not a whole number of representative elements
 * takes the element below, so the bound never cuts into a composed access.
 */
static long arena_floor(ValueDecl *rep, uint64_t unit)
{
	std::map<ValueDecl *, pet_arena_entry>::iterator it;
	long lo = 0;

	if (!unit)
		return 0;

	for (it = pet_arena_map.begin(); it != pet_arena_map.end(); ++it) {
		long off = it->second.offset;
		long e;

		if (it->second.rep != rep || it->first == rep)
			continue;
		e = off / (long) unit;
		if (off % (long) unit)
			e -= 1;
		if (e < lo)
			lo = e;
	}

	return lo;
}

/* The array reference at the root of "expr", looking through subscripts,
 * or NULL if there is none.
 *
 * The same shape as union_member_root: an access to an annotated array is
 * "lo[i]" or "A[i][j]", an ArraySubscriptExpr chain ending in a reference to
 * the declaration the pragma named.
 */
static ValueDecl *arena_array_root(Expr *expr)
{
	DeclRefExpr *ref;

	while (ArraySubscriptExpr *sub = dyn_cast<ArraySubscriptExpr>(expr))
		expr = sub->getBase()->IgnoreParenImpCasts();

	ref = dyn_cast<DeclRefExpr>(expr);
	if (!ref)
		return NULL;

	return ref->getDecl();
}

/* Is "expr" an access to an array that shares storage with another one?
 */
static ValueDecl *arena_member(Expr *expr, long *offset)
{
	ValueDecl *decl = arena_array_root(expr);

	if (!decl)
		return NULL;

	return pet_arena_lookup(decl, offset);
}

/* The byte stride of each subscript of "expr", outermost first.
 *
 * The stride of a subscript is the size of what it yields: for
 * "float (*A)[128]", "A[i]" is a float[128] and steps 512 bytes, while
 * "A[i][j]" is a float and steps 4.  Taken from the type rather than assumed,
 * because a two-dimensional buffer is exactly the case an annotation has to
 * carry -- ppcg builds no relation for a data-dependent row multiplied by a
 * stride, so those buffers are declared two-dimensional and must stay that
 * way.
 */
static void arena_strides(Expr *expr, ASTContext &ast_context,
	std::vector<ArraySubscriptExpr *> &subs, std::vector<uint64_t> &strides)
{
	while (ArraySubscriptExpr *sub = dyn_cast<ArraySubscriptExpr>(expr)) {
		subs.push_back(sub);
		strides.push_back(
		    ast_context.getTypeSizeInChars(sub->getType()).getQuantity());
		expr = sub->getBase()->IgnoreParenImpCasts();
	}
	std::reverse(subs.begin(), subs.end());
	std::reverse(strides.begin(), strides.end());
}

/* Construct the map that sends one element of the storage "expr" names onto
 * the "shift"th of the "scale" representative elements it spans.
 *
 *	A[i_0, ..., i_n] -> R[offset/unit + shift + sum_k i_k * stride_k/unit]
 *
 * which is affine in any number of dimensions, and that is the whole reason
 * this form survived where three others did not.  The C is untouched: the
 * array keeps its name, its type, its two-dimensional declaration and its
 * printed subscripts, and only the relation moves.  A cast in the access and
 * a flattened buffer both put the data-dependent index back under a
 * multiplication, which is the one thing ppcg cannot relate -- measured at
 * 402 nodes, where flattening took 316 parallel bands to 0.
 *
 * A MAP, NOT AN ACCESS, and that is the fix rather than a detail.  This built
 * a pet_expr whose subscript was a sum of extract_expr(idx) terms and handed
 * it to pet_expr_access_from_index HERE, at extraction, before the context
 * evaluation had filled in what those subscripts mean.  The relation came out
 * as the identity, losing the offset and the scale of every variable
 * subscript, so a write of a[i] landed on h[i] rather than h[2i] and nothing
 * conflicted with a read of h[2048]: the anti-dependence was never built and
 * the read crossed the write.  Measured on the arep probe, which returned
 * 16384 -- the low half of the 1026.0f another member had written over those
 * bytes -- and at 402 nodes, where five representatives lost a write, all
 * five of them arrays with no store of their own.
 *
 * Written as a map there is nothing left to evaluate: the strides and the
 * offset are compile-time constants and the member's own indices are the
 * map's input dimensions.  expr_collect_access applies it to the range once
 * the member's relation is exact.
 */
__isl_give isl_map *PetScan::arena_map(Expr *expr, ValueDecl *rep,
	long offset, int scale, int shift, __isl_keep isl_id *mid)
{
	std::vector<ArraySubscriptExpr *> subs;
	std::vector<uint64_t> strides;
	uint64_t unit = pointee_element_size(rep->getType(), ast_context);
	ValueDecl *member = arena_array_root(expr);
	isl_local_space *ls;
	isl_space *space;
	isl_aff *aff;
	isl_map *map;
	unsigned i;

	if (!unit || !member)
		return NULL;

	arena_strides(expr, ast_context, subs, strides);

	space = isl_space_alloc(ctx, 0, subs.size(), 1);
	/* THE MEMBER'S ID COMES FROM THE ACCESS, NOT FROM THE DECLARATION.
	 *
	 * isl compares tuple identity by isl_id, not by the name it prints.
	 * A fresh pet_id_from_decl prints "idx" exactly as the access's own
	 * range does and is a different id, so apply_range matched nothing
	 * and returned the empty map -- silently, which is how this cost a
	 * cycle to find rather than a compile error.  Measured:
	 * "before { S_1[] -> idx[o0] }", "map { idx[i0] -> h[o0] }",
	 * "after { }".
	 */
	space = isl_space_set_tuple_id(space, isl_dim_in, isl_id_copy(mid));
	space = isl_space_set_tuple_id(space, isl_dim_out,
				       pet_id_from_decl(ctx, rep));

	ls = isl_local_space_from_space(isl_space_domain(isl_space_copy(space)));
	aff = isl_aff_zero_on_domain(ls);
	aff = isl_aff_set_constant_si(aff,
				      (int) (offset / (long) unit + shift));
	for (i = 0; i < subs.size(); ++i)
		aff = isl_aff_set_coefficient_si(aff, isl_dim_in, i,
						 (int) (strides[i] / unit));

	/* THE RANGE HAS TO CARRY THE REPRESENTATIVE'S NAME.
	 *
	 * isl_map_from_aff takes its space from the aff, whose range is
	 * anonymous: the map came out as "lo[i0] -> [o0] : o0 = -8192 + 2i0",
	 * arithmetic right and destination nameless, so apply_range in
	 * expr_collect_access matched nothing and the composition led
	 * nowhere.  Measured on the aneg probe, which had been green and went
	 * red on exactly this.
	 */
	map = isl_map_from_aff(aff);
	map = isl_map_set_tuple_id(map, isl_dim_out,
				   isl_space_get_tuple_id(space, isl_dim_out));
	isl_space_free(space);

	return map;
}

/* How many representative elements does one element of "expr" cover?
 * 0 if this storage cannot be described, and the reason has been reported.
 *
 * Every boundary of the arena path is drawn here, in one place, so that a
 * shape which cannot be modelled costs a diagnostic and a non-zero exit
 * rather than a plausible wrong answer.  A guard that reports and then
 * carries on is worse than no guard: on the union path that produced four
 * warnings, exit 0, and an index read back as float bits.
 */
int PetScan::arena_scale(Expr *expr, ValueDecl *rep, long offset)
{
	std::vector<ArraySubscriptExpr *> subs;
	std::vector<uint64_t> strides;
	ValueDecl *decl = arena_array_root(expr);
	uint64_t unit = pointee_element_size(rep->getType(), ast_context);
	uint64_t mine = pointee_element_size(decl->getType(), ast_context);
	unsigned i;

	if (!unit || !mine)
		return 0;
	/* A NEGATIVE OFFSET IS ORDINARY, and only its remainder matters.
	 *
	 * This refused every negative offset outright, from the days when the
	 * representative had to sit at offset 0.  It no longer does: it is the
	 * member with the SMALLEST element, because an access through a
	 * smaller one would land inside one of its elements, and members that
	 * begin before it therefore have negative offsets.  The question is
	 * the same as for a positive offset -- does it divide into whole
	 * elements -- and the cast to uint64_t made -12288 enormous, so the
	 * guard rejected an offset that is exactly -6144 elements.
	 */
	if (offset % (long) unit) {
		report_arena(expr, decl->getName().str() + " is at byte offset "
			+ std::to_string(offset) + ", which is not a whole "
			"number of the " + std::to_string(unit) + "-byte "
			"elements of the array it shares storage with");
		return 0;
	}
	if (mine % unit) {
		report_arena(expr, "an element of " + decl->getName().str()
			+ " is " + std::to_string(mine) + " bytes, which is "
			"not a whole number of the " + std::to_string(unit)
			+ "-byte elements of the array it shares storage with");
		return 0;
	}

	arena_strides(expr, ast_context, subs, strides);
	for (i = 0; i < strides.size(); ++i)
		if (strides[i] % unit) {
			report_arena(expr, "dimension " + std::to_string(i)
				+ " of " + decl->getName().str() + " steps "
				+ std::to_string(strides[i]) + " bytes, which "
				"is not a whole number of the "
				+ std::to_string(unit) + "-byte elements of "
				"the array it shares storage with");
			return 0;
		}

	return mine / unit;
}

/* Construct an access pet_expr from "expr", an access to an array that shares
 * storage with another one.
 *
 * The INDEX keeps naming the array the source names, because the index is
 * what gets printed and the emitted C must come back unchanged.  The ACCESS
 * RELATION is replaced by one over the representative, because the relation
 * is what the dependence analysis reads.  pet keeps those two apart already,
 * which is what makes this possible without touching a single subscript in
 * the generated program.
 *
 * The relation covers a RANGE of representative elements when this array's
 * element is wider than the representative's: an f32 store over an f16
 * representative touches elements 2i and 2i+1, and naming only 2i understates
 * the write's footprint, which licenses exactly the reordering this exists to
 * forbid.
 */
__isl_give pet_expr *PetScan::access_from_arena(Expr *expr,
	__isl_take pet_expr *index, ValueDecl *rep, long offset)
{
	pet_expr *access;
	isl_union_map *arena;
	isl_multi_pw_aff *mpa;
	isl_id *mid;
	int scale, shift;

	access = pet_expr_access_from_index(expr->getType(), index,
					    ast_context);
	mpa = pet_expr_access_get_index(access);
	mid = mpa ? isl_multi_pw_aff_get_tuple_id(mpa, isl_dim_out) : NULL;
	isl_multi_pw_aff_free(mpa);
	scale = arena_scale(expr, rep, offset);
	arena = NULL;
	for (shift = 0; scale > 0 && shift < scale; ++shift) {
		isl_map *part = arena_map(expr, rep, offset, scale, shift, mid);

		if (!part)
			break;
		arena = arena ?
		    isl_union_map_union(arena, isl_union_map_from_map(part)) :
		    isl_union_map_from_map(part);
	}
	isl_id_free(mid);
	if (getenv("PET_DEBUG_EXTENT") &&
	    pet_debug_only(rep->getNameAsString().c_str())) {
		char *s = arena ? isl_union_map_to_str(arena) : NULL;

		fprintf(stderr, "arena %s <- offset %ld scale %d shift %d : %s\n",
			rep->getNameAsString().c_str(), offset, scale, shift,
			s ? s : "NO MAP");
		free(s);
	}
	if (!access || scale <= 0 || shift != scale || !arena) {
		pet_expr_free(access);
		isl_union_map_free(arena);
		return NULL;
	}

	/* THE COMPOSITION TRAVELS AS DATA, not as an installed relation.
	 *
	 * The member's own slots are left exactly as they are: the context
	 * evaluation fills them from the evaluated index, and they are right.
	 * What used to happen here was to overwrite them with a relation
	 * built at THIS moment, from subscripts nobody had evaluated yet --
	 * so it collapsed to the identity and lost the offset and the scale.
	 * expr_collect_access applies these maps to the range instead, when
	 * the member's relation is exact, and skips them for the plain view
	 * so that liveness keeps judging the array the source names.
	 */
	access = pet_expr_access_set_arena(access, arena);

	return access;
}

__isl_give pet_expr *PetScan::extract_access_expr(Expr *expr)
{
	pet_expr *index, *access;
	ValueDecl *rep;
	long offset = 0;

	index = extract_index_expr(expr);

	if (pet_expr_get_type(index) == pet_expr_int)
		return index;

	if (union_field(expr))
		return access_from_union_member(expr, index);

	rep = arena_member(expr, &offset);
	if (rep)
		return access_from_arena(expr, index, rep, offset);

	return pet_expr_access_from_index(expr->getType(), index, ast_context);
}

/* Extract an index expression from "decl" and then convert it into
 * an access pet_expr.
 */
__isl_give pet_expr *PetScan::extract_access_expr(ValueDecl *decl)
{
	return pet_expr_access_from_index(decl->getType(),
					extract_index_expr(decl), ast_context);
}

__isl_give pet_expr *PetScan::extract_expr(ParenExpr *expr)
{
	return extract_expr(expr->getSubExpr());
}

/* Extract an assume statement from the argument "expr"
 * of a __builtin_assume or __pencil_assume statement.
 */
__isl_give pet_expr *PetScan::extract_assume(Expr *expr)
{
	return pet_expr_new_unary(0, pet_op_assume, extract_expr(expr));
}

/* If "expr" is an address-of operator, then return its argument.
 * Otherwise, return NULL.
 */
static Expr *extract_addr_of_arg(Expr *expr)
{
	UnaryOperator *op;

	if (expr->getStmtClass() != Stmt::UnaryOperatorClass)
		return NULL;
	op = cast<UnaryOperator>(expr);
	if (op->getOpcode() != UO_AddrOf)
		return NULL;
	return op->getSubExpr();
}

/* If "expr" is a pointer with an integer added to it, return the
 * subscript naming the same place, and NULL otherwise.
 *
 * "p + i" and "&p[i]" are one address written two ways, and only the
 * second was an access as far as a scop was concerned: the first
 * reached the test for one as a BinaryOperator and the call taking it
 * was refused.  A graph of an inference engine hands its kernels
 * "(const block_q8_0 *) (w + t*4352)" twenty-four times over.
 *
 * The subscript is built and given to the reader that already exists,
 * rather than the access being assembled here.  Assembling it here is
 * what a first attempt did and it produced an access of one dimension
 * more than the same argument written "&w[i*i]": the base already
 * carried a dimension and a subscript went on top of it.  Two builders
 * of one representation disagree sooner or later, and silently -- it
 * shows up as a scop that is subtly the wrong shape.  So there is one
 * builder and this hands it its usual input.
 *
 * Whether the offset is affine is not asked, because a subscript does
 * not ask it either: that question is answered further along and
 * answered alike for both spellings, which is the point of writing one
 * as the other.
 */
static Expr *subscript_from_pointer_offset(ASTContext &ast_context,
	Expr *expr)
{
	BinaryOperator *bin = dyn_cast<BinaryOperator>(expr);
	Expr *base, *offset;

	if (!bin || bin->getOpcode() != BO_Add)
		return NULL;

	if (bin->getLHS()->getType()->isPointerType() &&
	    bin->getRHS()->getType()->isIntegerType()) {
		base = bin->getLHS();
		offset = bin->getRHS();
	} else if (bin->getRHS()->getType()->isPointerType() &&
		   bin->getLHS()->getType()->isIntegerType()) {
		base = bin->getRHS();
		offset = bin->getLHS();
	} else {
		return NULL;
	}

	return new (ast_context) ArraySubscriptExpr(base, offset,
			base->getType()->getPointeeType(),
			VK_LValue, OK_Ordinary, expr->getEndLoc());
}

/* Construct a pet_expr corresponding to the function call argument "expr".
 * The argument appears in position "pos" of a call to function "fd".
 *
 * If we are passing along a pointer to an array element
 * or an entire row or even higher dimensional slice of an array,
 * then the function being called may write into the array.
 *
 * We assume here that if the function is declared to take a pointer
 * to a const type, then the function may only perform a read
 * and that otherwise, it may either perform a read or a write (or both).
 * We only perform this check if "detect_writes" is set.
 */
__isl_give pet_expr *PetScan::extract_argument(FunctionDecl *fd, int pos,
	Expr *expr, bool detect_writes)
{
	Expr *arg;
	pet_expr *res;
	int is_addr = 0, is_partial = 0;

	expr = pet_clang_strip_casts(expr);
	/* "p + i" names the same place as "&p[i]", and the call taking it
	 * is the one that has to be told so.  Without this the base is
	 * built from "p" alone and comes out with no dimension at all,
	 * which the patching of an inlined body then tries to extend by
	 * minus one: the dimension count is unsigned and the subtraction
	 * wraps.  isl catches it -- "space->n_out <= n_out" -- and the
	 * inlining of that body quietly does not happen.
	 *
	 * The same subscript builder answers here as answers on the path
	 * for an argument bound to an array parameter.  It has to be both
	 * places because the two paths are taken by different calls: a
	 * body that can be put in place goes one way, a call left standing
	 * goes the other, and "ggml_vec_dot_f32(n, &qk, 0, q + h*qs, ...)"
	 * inside an inlined indexer is the second inside the first.
	 */
	arg = subscript_from_pointer_offset(ast_context, expr);
	if (arg) {
		is_addr = 1;
		expr = arg;
	} else if ((arg = extract_addr_of_arg(expr))) {
		is_addr = 1;
		expr = arg;
	}
	res = extract_expr(expr);
	if (!res)
		return NULL;
	if (pet_clang_array_depth(expr->getType()) > 0)
		is_partial = 1;
	if (detect_writes && (is_addr || is_partial) &&
	    pet_expr_get_type(res) == pet_expr_access) {
		ParmVarDecl *parm;
		if (!fd->hasPrototype()) {
			report_prototype_required(expr);
			return pet_expr_free(res);
		}
		/* An argument passed through the ellipsis of a variadic
		 * function has no parameter it corresponds to, so nothing
		 * says it is only read.
		 */
		if (pos >= fd->getNumParams()) {
			res = mark_may_write(res);
			goto done;
		}
		parm = fd->getParamDecl(pos);
		if (!const_base(parm->getType()))
			res = mark_may_write(res);
	}

done:
	if (is_addr)
		res = pet_expr_new_unary(0, pet_op_address_of, res);
	return res;
}

/* Find the first FunctionDecl with the given name.
 * "call" is the corresponding call expression and is only used
 * for reporting errors.
 *
 * Return NULL on error.
 */
FunctionDecl *PetScan::find_decl_from_name(CallExpr *call, string name)
{
	TranslationUnitDecl *tu = ast_context.getTranslationUnitDecl();
	DeclContext::decl_iterator begin = tu->decls_begin();
	DeclContext::decl_iterator end = tu->decls_end();
	for (DeclContext::decl_iterator i = begin; i != end; ++i) {
		FunctionDecl *fd = dyn_cast<FunctionDecl>(*i);
		if (!fd)
			continue;
		if (fd->getNameAsString().compare(name) != 0)
			continue;
		if (fd->hasBody())
			return fd;
		report_missing_summary_function_body(call);
		return NULL;
	}
	report_missing_summary_function(call);
	return NULL;
}

/* Return the FunctionDecl for the summary function associated to the
 * function called by "call".
 *
 * In particular, if the pencil option is set, then
 * search for an annotate attribute formatted as
 * "pencil_access(name)", where "name" is the name of the summary function.
 *
 * If no summary function was specified, then return the FunctionDecl
 * that is actually being called.
 *
 * Return NULL on error.
 */
FunctionDecl *PetScan::get_summary_function(CallExpr *call)
{
	FunctionDecl *decl = pet_clang_direct_callee(call);
	if (!decl)
		return NULL;

	if (!options->pencil)
		return decl;

	specific_attr_iterator<AnnotateAttr> begin, end, i;
	begin = decl->specific_attr_begin<AnnotateAttr>();
	end = decl->specific_attr_end<AnnotateAttr>();
	for (i = begin; i != end; ++i) {
		string attr = (*i)->getAnnotation().str();

		const char prefix[] = "pencil_access(";
		size_t start = attr.find(prefix);
		if (start == string::npos)
			continue;
		start += strlen(prefix);
		string name = attr.substr(start, attr.find(')') - start);

		return find_decl_from_name(call, name);
	}

	return decl;
}

/* Is "name" the name of an assume statement?
 * "pencil" indicates whether pencil builtins and pragmas should be supported.
 * "__builtin_assume" is always accepted.
 * If "pencil" is set, then "__pencil_assume" is also accepted.
 */
static bool is_assume(int pencil, const string &name)
{
	if (name == "__builtin_assume")
		return true;
	return pencil && name == "__pencil_assume";
}

/* Construct a pet_expr representing a function call.
 *
 * If this->call2id is not NULL and it contains a mapping for this call,
 * then this means that the corresponding function has been inlined.
 * Return a pet_expr that reads from the variable that
 * stores the return value of the inlined call.
 *
 * In the special case of a "call" to __builtin_assume or __pencil_assume,
 * construct an assume expression instead.
 *
 * In the case of a "call" to __pencil_kill, the arguments
 * are neither read nor written (only killed), so there
 * is no need to check for writes to these arguments.
 *
 * __pencil_assume and __pencil_kill are only recognized
 * when the pencil option is set.
 */
__isl_give pet_expr *PetScan::extract_expr(CallExpr *expr)
{
	pet_expr *res = NULL;
	FunctionDecl *fd;
	string name;
	unsigned n_arg;
	bool is_kill;

	if (call2id && call2id->find(expr) != call2id->end())
		return pet_expr_access_from_id(isl_id_copy(call2id[0][expr]),
						ast_context);

	fd = pet_clang_direct_callee(expr);
	if (!fd) {
		unsupported(expr);
		return NULL;
	}

	name = fd->getDeclName().getAsString();
	n_arg = expr->getNumArgs();

	if (n_arg == 1 && is_assume(options->pencil, name))
		return extract_assume(expr->getArg(0));
	is_kill = options->pencil && name == "__pencil_kill";

	res = pet_expr_new_call(ctx, name.c_str(), n_arg);
	if (!res)
		return NULL;

	for (unsigned i = 0; i < n_arg; ++i) {
		Expr *arg = expr->getArg(i);
		res = pet_expr_set_arg(res, i,
			    PetScan::extract_argument(fd, i, arg, !is_kill));
	}

	fd = get_summary_function(expr);
	if (!fd)
		return pet_expr_free(res);

	res = set_summary(res, fd);

	return res;
}

/* Construct a pet_expr representing a (C style) cast.
 *
 * A cast whose written type is, after canonicalisation and after dropping
 * qualifiers, the type the operand already has is not a conversion: it
 * computes the operand.  Such a cast is therefore extracted as its operand,
 * and the pet_expr_cast node is not built.
 *
 * This is not a simplification for its own sake.  A pet_expr_cast is opaque
 * to the parts of pet that must look through an expression: an index
 * subscript that is a cast makes pet_stmt_can_build_ast_exprs answer no
 * ("argument %d of this access is not itself an access"), and the whole scop
 * is then passed through unscheduled.  The idiom that meets it is a table
 * lookup keyed by a narrow integer, of which ggml has three written down side
 * by side -- simd-mappings.h:41 ggml_table_f32_e8m0_half[(uint8_t)(x)],
 * :48 ggml_table_f32_ue4m3[(uint8_t)(x)], and the f16 table reached the same
 * way -- where the operand is ALREADY uint8_t/uint16_t and the cast says so
 * rather than doing anything.  Dropping those casts costs no value and makes
 * the subscript an access, which is what the printer needs.
 *
 * A cast that does convert -- a narrowing, a signedness change, a
 * float/integer conversion -- fails this test and is still built as a cast,
 * so nothing that changes a value is silently discarded.
 */
__isl_give pet_expr *PetScan::extract_expr(CStyleCastExpr *expr)
{
	pet_expr *arg;
	QualType type, dst, src;

	arg = extract_expr(expr->getSubExpr());
	if (!arg)
		return NULL;

	dst = ast_context.getCanonicalType(expr->getType()).getUnqualifiedType();
	src = ast_context.getCanonicalType(
		expr->getSubExpr()->getType()).getUnqualifiedType();
	if (dst == src)
		return arg;

	type = expr->getTypeAsWritten();
	return pet_expr_new_cast(type.getAsString().c_str(), arg);
}

/* Construct a pet_expr representing an integer.
 */
__isl_give pet_expr *PetScan::extract_expr(IntegerLiteral *expr)
{
	return pet_expr_new_int(extract_int(expr));
}

/* Construct a pet_expr representing the integer enum constant "ecd".
 */
__isl_give pet_expr *PetScan::extract_expr(EnumConstantDecl *ecd)
{
	isl_val *v;
	const llvm::APSInt &init = ecd->getInitVal();
	v = ::extract_int(ctx, init.isSigned(), init);
	return pet_expr_new_int(v);
}

/* Try and construct a pet_expr representing "expr".
 */
__isl_give pet_expr *PetScan::extract_expr(Expr *expr)
{
	switch (expr->getStmtClass()) {
	case Stmt::UnaryOperatorClass:
		return extract_expr(cast<UnaryOperator>(expr));
	case Stmt::CompoundAssignOperatorClass:
	case Stmt::BinaryOperatorClass:
		return extract_expr(cast<BinaryOperator>(expr));
	case Stmt::ImplicitCastExprClass:
		return extract_expr(cast<ImplicitCastExpr>(expr));
	case Stmt::ArraySubscriptExprClass:
	case Stmt::DeclRefExprClass:
	case Stmt::MemberExprClass:
		return extract_access_expr(expr);
	case Stmt::IntegerLiteralClass:
		return extract_expr(cast<IntegerLiteral>(expr));
	case Stmt::FloatingLiteralClass:
		return extract_expr(cast<FloatingLiteral>(expr));
	case Stmt::ParenExprClass:
		return extract_expr(cast<ParenExpr>(expr));
	case Stmt::ConstantExprClass:
		return extract_expr(cast<ConstantExpr>(expr)->getSubExpr());
	case Stmt::TypeTraitExprClass: {
		TypeTraitExpr *tt = cast<TypeTraitExpr>(expr);

		/* A trait that answers with something other than yes or no
		 * is not a value a scop can hold, and says so.
		 */
		if (!tt->isValueDependent() && tt->isStoredAsBoolean())
			return pet_expr_new_int(isl_val_int_from_si(ctx,
						tt->getBoolValue() ? 1 : 0));
		unsupported(expr);
		return NULL;
	}
	case Stmt::CXXBoolLiteralExprClass:
		return pet_expr_new_int(isl_val_int_from_si(ctx,
			cast<CXXBoolLiteralExpr>(expr)->getValue() ? 1 : 0));
	case Stmt::ConditionalOperatorClass:
		return extract_expr(cast<ConditionalOperator>(expr));
	case Stmt::CallExprClass:
	case Stmt::CXXMemberCallExprClass:
	case Stmt::CXXOperatorCallExprClass:
		return extract_expr(cast<CallExpr>(expr));
	case Stmt::CStyleCastExprClass:
		return extract_expr(cast<CStyleCastExpr>(expr));
	default:
		unsupported(expr);
	}
	return NULL;
}

/* Check if the given initialization statement is an assignment.
 * If so, return that assignment.  Otherwise return NULL.
 */
BinaryOperator *PetScan::initialization_assignment(Stmt *init)
{
	BinaryOperator *ass;

	if (init->getStmtClass() != Stmt::BinaryOperatorClass)
		return NULL;

	ass = cast<BinaryOperator>(init);
	if (ass->getOpcode() != BO_Assign)
		return NULL;

	return ass;
}

/* Check if the given initialization statement is a declaration
 * of a single variable.
 * If so, return that declaration.  Otherwise return NULL.
 */
Decl *PetScan::initialization_declaration(Stmt *init)
{
	DeclStmt *decl;

	if (init->getStmtClass() != Stmt::DeclStmtClass)
		return NULL;

	decl = cast<DeclStmt>(init);

	if (!decl->isSingleDecl())
		return NULL;

	return decl->getSingleDecl();
}

/* Given the assignment operator in the initialization of a for loop,
 * extract the induction variable, i.e., the (integer)variable being
 * assigned.
 */
ValueDecl *PetScan::extract_induction_variable(BinaryOperator *init)
{
	Expr *lhs;
	DeclRefExpr *ref;
	ValueDecl *decl;
	const Type *type;

	lhs = init->getLHS();
	if (lhs->getStmtClass() != Stmt::DeclRefExprClass) {
		unsupported(init);
		return NULL;
	}

	ref = cast<DeclRefExpr>(lhs);
	decl = ref->getDecl();
	type = decl->getType().getTypePtr();

	if (!type->isIntegerType()) {
		unsupported(lhs);
		return NULL;
	}

	return decl;
}

/* Given the initialization statement of a for loop and the single
 * declaration in this initialization statement,
 * extract the induction variable, i.e., the (integer) variable being
 * declared.
 */
VarDecl *PetScan::extract_induction_variable(Stmt *init, Decl *decl)
{
	VarDecl *vd;

	vd = cast<VarDecl>(decl);

	const QualType type = vd->getType();
	if (!type->isIntegerType()) {
		unsupported(init);
		return NULL;
	}

	if (!vd->getInit()) {
		unsupported(init);
		return NULL;
	}

	return vd;
}

/* Check that op is of the form iv++ or iv--.
 * Return a pet_expr representing "1" or "-1" accordingly.
 */
__isl_give pet_expr *PetScan::extract_unary_increment(
	clang::UnaryOperator *op, clang::ValueDecl *iv)
{
	Expr *sub;
	DeclRefExpr *ref;
	isl_val *v;

	if (!op->isIncrementDecrementOp()) {
		unsupported(op);
		return NULL;
	}

	sub = op->getSubExpr();
	if (sub->getStmtClass() != Stmt::DeclRefExprClass) {
		unsupported(op);
		return NULL;
	}

	ref = cast<DeclRefExpr>(sub);
	if (ref->getDecl() != iv) {
		unsupported(op);
		return NULL;
	}

	if (op->isIncrementOp())
		v = isl_val_one(ctx);
	else
		v = isl_val_negone(ctx);

	return pet_expr_new_int(v);
}

/* Check if op is of the form
 *
 *	iv = expr
 *
 * and return the increment "expr - iv" as a pet_expr.
 */
__isl_give pet_expr *PetScan::extract_binary_increment(BinaryOperator *op,
	clang::ValueDecl *iv)
{
	int type_size;
	Expr *lhs;
	DeclRefExpr *ref;
	pet_expr *expr, *expr_iv;

	if (op->getOpcode() != BO_Assign) {
		unsupported(op);
		return NULL;
	}

	lhs = op->getLHS();
	if (lhs->getStmtClass() != Stmt::DeclRefExprClass) {
		unsupported(op);
		return NULL;
	}

	ref = cast<DeclRefExpr>(lhs);
	if (ref->getDecl() != iv) {
		unsupported(op);
		return NULL;
	}

	expr = extract_expr(op->getRHS());
	expr_iv = extract_expr(lhs);

	type_size = pet_clang_get_type_size(iv->getType(), ast_context);
	return pet_expr_new_binary(type_size, pet_op_sub, expr, expr_iv);
}

/* Check that op is of the form iv += cst or iv -= cst
 * and return a pet_expr corresponding to cst or -cst accordingly.
 */
__isl_give pet_expr *PetScan::extract_compound_increment(
	CompoundAssignOperator *op, clang::ValueDecl *iv)
{
	Expr *lhs;
	DeclRefExpr *ref;
	bool neg = false;
	pet_expr *expr;
	BinaryOperatorKind opcode;

	opcode = op->getOpcode();
	if (opcode != BO_AddAssign && opcode != BO_SubAssign) {
		unsupported(op);
		return NULL;
	}
	if (opcode == BO_SubAssign)
		neg = true;

	lhs = op->getLHS();
	if (lhs->getStmtClass() != Stmt::DeclRefExprClass) {
		unsupported(op);
		return NULL;
	}

	ref = cast<DeclRefExpr>(lhs);
	if (ref->getDecl() != iv) {
		unsupported(op);
		return NULL;
	}

	expr = extract_expr(op->getRHS());
	if (neg) {
		int type_size;
		type_size = pet_clang_get_type_size(op->getType(), ast_context);
		expr = pet_expr_new_unary(type_size, pet_op_minus, expr);
	}

	return expr;
}

/* Check that the increment of the given for loop increments
 * (or decrements) the induction variable "iv" and return
 * the increment as a pet_expr if successful.
 */
__isl_give pet_expr *PetScan::extract_increment(clang::ForStmt *stmt,
	ValueDecl *iv)
{
	Stmt *inc = stmt->getInc();

	if (!inc) {
		report_missing_increment(stmt);
		return NULL;
	}

	if (inc->getStmtClass() == Stmt::UnaryOperatorClass)
		return extract_unary_increment(cast<UnaryOperator>(inc), iv);
	if (inc->getStmtClass() == Stmt::CompoundAssignOperatorClass)
		return extract_compound_increment(
				cast<CompoundAssignOperator>(inc), iv);
	if (inc->getStmtClass() == Stmt::BinaryOperatorClass)
		return extract_binary_increment(cast<BinaryOperator>(inc), iv);

	unsupported(inc);
	return NULL;
}

/* Construct a pet_tree for a while loop.
 *
 * If we were only able to extract part of the body, then simply
 * return that part.
 */
__isl_give pet_tree *PetScan::extract(WhileStmt *stmt)
{
	pet_expr *pe_cond;
	pet_tree *tree;

	tree = extract(stmt->getBody());
	if (partial)
		return tree;
	pe_cond = extract_expr(stmt->getCond());
	tree = pet_tree_new_while(pe_cond, tree);

	return tree;
}

/* Construct a pet_tree for a for statement.
 * The for loop is required to be of one of the following forms
 *
 *	for (i = init; condition; ++i)
 *	for (i = init; condition; --i)
 *	for (i = init; condition; i += constant)
 *	for (i = init; condition; i -= constant)
 *
 * We extract a pet_tree for the body and then include it in a pet_tree
 * of type pet_tree_for.
 *
 * As a special case, we also allow a for loop of the form
 *
 *	for (;;)
 *
 * in which case we return a pet_tree of type pet_tree_infinite_loop.
 *
 * If we were only able to extract part of the body, then simply
 * return that part.
 */
__isl_give pet_tree *PetScan::extract_for(ForStmt *stmt)
{
	BinaryOperator *ass;
	Decl *decl;
	Stmt *init;
	Expr *rhs;
	ValueDecl *iv;
	pet_tree *tree;
	int independent;
	int declared;
	pet_expr *pe_init, *pe_inc, *pe_iv, *pe_cond;

	independent = is_current_stmt_marked_independent();

	if (!stmt->getInit() && !stmt->getCond() && !stmt->getInc()) {
		tree = extract(stmt->getBody());
		if (partial)
			return tree;
		tree = pet_tree_new_infinite_loop(tree);
		return tree;
	}

	init = stmt->getInit();
	if (!init) {
		unsupported(stmt);
		return NULL;
	}
	if ((ass = initialization_assignment(init)) != NULL) {
		iv = extract_induction_variable(ass);
		if (!iv)
			return NULL;
		rhs = ass->getRHS();
	} else if ((decl = initialization_declaration(init)) != NULL) {
		VarDecl *var = extract_induction_variable(init, decl);
		if (!var)
			return NULL;
		iv = var;
		rhs = var->getInit();
		/* AN ITERATOR IS A DECLARATION LIKE ANY OTHER.
		 *
		 * Renaming happens in extract(CompoundStmt *): every VarDecl
		 * that reached `declarations` is checked against the names in
		 * use and given a suffix if it collides, and the substituter
		 * then carries the new name into the declaration and into
		 * every access, which is why n_1 and acc_4 come out right.
		 * An iterator declared in a for-init never reached that list:
		 * it is taken here and handed straight to extract_access_expr.
		 *
		 * With bodies inlined that is not a curiosity.  Each body
		 * brings its own `for (int64_t i = ...)`, and a scop that
		 * inlines forty-four of them declares `i` forty-four times in
		 * one scope -- measured on an emitted transformer, where the
		 * scheduled output would not compile: "redeclaration of 'i'
		 * with no linkage".  Offering the iterator to the same path
		 * costs nothing when there is no collision and renames it
		 * when there is.
		 */
		declarations.push_back(var);
	} else {
		unsupported(stmt->getInit());
		return NULL;
	}

	declared = !initialization_assignment(stmt->getInit());
	tree = extract(stmt->getBody());
	if (partial)
		return tree;
	pe_iv = extract_access_expr(iv);
	pe_iv = mark_write(pe_iv);
	pe_init = extract_expr(rhs);
	if (!stmt->getCond())
		pe_cond = pet_expr_new_int(isl_val_one(ctx));
	else
		pe_cond = extract_expr(stmt->getCond());
	pe_inc = extract_increment(stmt, iv);
	tree = pet_tree_new_for(independent, declared, pe_iv, pe_init, pe_cond,
				pe_inc, tree);
	return tree;
}

/* Store the names of the variables declared in decl_context
 * in the set declared_names.  Make sure to only do this once by
 * setting declared_names_collected.
 */
void PetScan::collect_declared_names()
{
	DeclContext *DC = decl_context;
	DeclContext::decl_iterator it;

	if (declared_names_collected)
		return;

	for (it = DC->decls_begin(); it != DC->decls_end(); ++it) {
		Decl *D = *it;
		NamedDecl *named;

		if (!isa<NamedDecl>(D))
			continue;
		named = cast<NamedDecl>(D);
		declared_names.insert(named->getNameAsString());
	}

	declared_names_collected = true;
}

/* Add the names in "names" that are not also in this->declared_names
 * to this->used_names.
 * It is up to the caller to make sure that declared_names has been
 * populated, if needed.
 */
void PetScan::add_new_used_names(const std::set<std::string> &names)
{
	std::set<std::string>::const_iterator it;

	for (it = names.begin(); it != names.end(); ++it) {
		if (declared_names.find(*it) != declared_names.end())
			continue;
		used_names.insert(*it);
	}
}

/* What does "DC" call the things it holds?
 *
 * Worked out once and kept on the walk, since every scan of every body
 * put in place of a call asks the same of the same contexts.
 */
static const struct pet_walk::context_names &names_of_context(
	struct pet_walk *walk, DeclContext *DC)
{
	std::map<DeclContext *, struct pet_walk::context_names>::iterator it;
	DeclContext::decl_iterator d;

	it = walk->names_of.find(DC);
	if (it != walk->names_of.end())
		return it->second;

	struct pet_walk::context_names &names = walk->names_of[DC];

	for (d = DC->decls_begin(); d != DC->decls_end(); ++d) {
		NamedDecl *named = dyn_cast<NamedDecl>(*d);

		if (!named)
			continue;
		std::string name = named->getNameAsString();
		if (names.first.count(name))
			names.many.insert(name);
		else
			names.first[name] = *d;
	}

	return names;
}

/* Is the name "name" used in any declaration other than "decl"?
 *
 * If the name was found to be in use before, the consider it to be in use.
 * Otherwise, check the DeclContext of the function containing the scop
 * as well as all ancestors of this DeclContext for declarations
 * other than "decl" that declare something called "name".
 *
 * What each context calls the things it holds is asked of the walk,
 * which works it out once.  Walking the declarations here instead is
 * a walk of the translation unit, and after a link that is every
 * declaration of every unit: profiled over 49 units of llama-dspark,
 * the whole of the run sat in this function, reached from
 * generate_new_name for the arguments of bodies being put in place.
 */
bool PetScan::name_in_use(const string &name, Decl *decl)
{
	DeclContext *DC;

	if (used_names.find(name) != used_names.end())
		return true;

	for (DC = decl_context; DC; DC = DC->getParent()) {
		const struct pet_walk::context_names &names =
					names_of_context(walk, DC);
		std::map<std::string, Decl *>::const_iterator it;

		if (names.many.find(name) != names.many.end())
			return true;
		it = names.first.find(name);
		if (it != names.first.end() && it->second != decl)
			return true;
	}

	return false;
}

/* Generate a new name based on "name" that is not in use.
 * Do so by adding a suffix _i, with i an integer.
 */
string PetScan::generate_new_name(const string &name)
{
	string new_name;

	do {
		std::ostringstream oss;
		oss << name << "_" << n_rename++;
		new_name = oss.str();
	} while (name_in_use(new_name, NULL));

	return new_name;
}

/* Try and construct a pet_tree corresponding to a compound statement.
 *
 * "skip_declarations" is set if we should skip initial declarations
 * in the children of the compound statements.
 *
 * Collect a new set of declarations for the current compound statement.
 * If any of the names in these declarations is also used by another
 * declaration reachable from the current function, then rename it
 * to a name that is not already in use.
 * In particular, keep track of the old and new names in a pet_substituter
 * and apply the substitutions to the pet_tree corresponding to the
 * compound statement.
 */
__isl_give pet_tree *PetScan::extract(CompoundStmt *stmt,
	bool skip_declarations)
{
	pet_tree *tree;
	std::vector<VarDecl *> saved_declarations;
	std::vector<VarDecl *>::iterator it;
	pet_substituter substituter;

	saved_declarations = declarations;
	declarations.clear();
	tree = extract(stmt->children(), true, skip_declarations, stmt);
	for (it = declarations.begin(); it != declarations.end(); ++it) {
		isl_id *id;
		pet_expr *expr;
		VarDecl *decl = *it;
		string name = decl->getNameAsString();
		bool in_use = name_in_use(name, decl);

		used_names.insert(name);
		if (!in_use)
			continue;

		name = generate_new_name(name);
		id = pet_id_from_name_and_decl(ctx, name.c_str(), decl);
		expr = pet_expr_access_from_id(id, ast_context);
		id = pet_id_from_decl(ctx, decl);
		substituter.add_sub(id, expr);
		used_names.insert(name);
	}
	tree = substituter.substitute(tree);
	declarations = saved_declarations;

	return tree;
}

/* Return the file offset of the expansion location of "Loc".
 */
static unsigned getExpansionOffset(SourceManager &SM, SourceLocation Loc)
{
	return SM.getFileOffset(SM.getExpansionLoc(Loc));
}

#ifdef HAVE_FINDLOCATIONAFTERTOKEN

/* Return a SourceLocation for the location after the first semicolon
 * after "loc".  If Lexer::findLocationAfterToken is available, we simply
 * call it and also skip trailing spaces and newline.
 */
static SourceLocation location_after_semi(SourceLocation loc, SourceManager &SM,
	const LangOptions &LO)
{
	return Lexer::findLocationAfterToken(loc, tok::semi, SM, LO, true);
}

#else

/* Return a SourceLocation for the location after the first semicolon
 * after "loc".  If Lexer::findLocationAfterToken is not available,
 * we look in the underlying character data for the first semicolon.
 */
static SourceLocation location_after_semi(SourceLocation loc, SourceManager &SM,
	const LangOptions &LO)
{
	const char *semi;
	const char *s = SM.getCharacterData(loc);

	semi = strchr(s, ';');
	if (!semi)
		return SourceLocation();
	return loc.getFileLocWithOffset(semi + 1 - s);
}

#endif

/* If the token at "loc" is the first token on the line, then return
 * a location referring to the start of the line and set *indent
 * to the indentation of "loc"
 * Otherwise, return "loc" and set *indent to "".
 *
 * This function is used to extend a scop to the start of the line
 * if the first token of the scop is also the first token on the line.
 *
 * We look for the first token on the line.  If its location is equal to "loc",
 * then the latter is the location of the first token on the line.
 */
static SourceLocation move_to_start_of_line_if_first_token(SourceLocation loc,
	SourceManager &SM, const LangOptions &LO, char **indent)
{
	std::pair<FileID, unsigned> file_offset_pair;
	llvm::StringRef file;
	const char *pos;
	Token tok;
	SourceLocation token_loc, line_loc;
	int col;
	const char *s;

	loc = SM.getExpansionLoc(loc);
	col = SM.getExpansionColumnNumber(loc);
	line_loc = loc.getLocWithOffset(1 - col);
	file_offset_pair = SM.getDecomposedLoc(line_loc);
	file = SM.getBufferData(file_offset_pair.first, NULL);
	pos = file.data() + file_offset_pair.second;

	Lexer lexer(SM.getLocForStartOfFile(file_offset_pair.first), LO,
					file.begin(), pos, file.end());
	lexer.LexFromRawLexer(tok);
	token_loc = tok.getLocation();

	s = SM.getCharacterData(line_loc);
	*indent = strndup(s, token_loc == loc ? col - 1 : 0);

	if (token_loc == loc)
		return line_loc;
	else
		return loc;
}

/* Construct a pet_loc corresponding to the region covered by "range".
 * If "skip_semi" is set, then we assume "range" is followed by
 * a semicolon and also include this semicolon.
 */
__isl_give pet_loc *PetScan::construct_pet_loc(SourceRange range,
	bool skip_semi)
{
	SourceLocation loc = range.getBegin();
	SourceManager &SM = PP.getSourceManager();
	const LangOptions &LO = PP.getLangOpts();
	int line = PP.getSourceManager().getExpansionLineNumber(loc);
	unsigned start, end;
	char *indent;

	loc = move_to_start_of_line_if_first_token(loc, SM, LO, &indent);
	start = getExpansionOffset(SM, loc);
	loc = range.getEnd();
	if (skip_semi)
		loc = location_after_semi(loc, SM, LO);
	else
		loc = PP.getLocForEndOfToken(loc);
	end = getExpansionOffset(SM, loc);

	/* The name of the file this region was written in, so that the
	 * region can be printed back from it.  A StringRef is not
	 * NUL-terminated by contract, so it is turned into a std::string
	 * that outlives the call rather than handed over as it is.  A
	 * location that names no file leaves the loc without one, and
	 * printing the original text of such a region fails loudly.
	 */
	std::string filename = SM.getFilename(loc).str();

	return pet_loc_alloc(ctx, start, end, line, indent,
				filename.empty() ? NULL : filename.c_str());
}

/* Convert a top-level pet_expr to an expression pet_tree.
 */
__isl_give pet_tree *PetScan::extract(__isl_take pet_expr *expr,
	SourceRange range, bool skip_semi)
{
	pet_loc *loc;
	pet_tree *tree;

	tree = pet_tree_new_expr(expr);
	loc = construct_pet_loc(range, skip_semi);
	tree = pet_tree_set_loc(tree, loc);

	return tree;
}

/* Construct a pet_tree for an if statement.
 */
/* Is "stmt" a call from which control never comes back?
 */
static bool is_trap_call(Stmt *stmt)
{
	CallExpr *call;
	FunctionDecl *fd;

	call = stmt ? dyn_cast<CallExpr>(stmt) : NULL;
	if (!call)
		return false;
	fd = pet_clang_direct_callee(call);

	return fd && fd->isNoReturn();
}

/* Does an execution that enters "stmt" never leave it again?
 *
 * A compound is left only by reaching its end, so one that holds a trap
 * anywhere is never left either.  This asks about the whole of "stmt",
 * which is not the same as asking whether "stmt" contributes nothing:
 * the statements a compound runs before it traps are still run.
 */
static bool traps(Stmt *stmt)
{
	CompoundStmt *c;

	if (!stmt)
		return false;

	c = dyn_cast<CompoundStmt>(stmt);
	if (c) {
		StmtIterator i;
		StmtRange range = c->children();

		for (i = range.first; i != range.second; ++i)
			if (traps(*i))
				return true;
		return false;
	}

	return is_trap_call(stmt);
}

/* Construct a pet_tree for an if statement.
 *
 * An if whose only business is to trap is not a branch.  Every execution
 * that reaches the statement after it took none of what the branch
 * holds, so the branch contributes no read, no write and no place in
 * the schedule: it is not part of what the scop describes.  Neither is
 * the condition, which is why it is not even looked at -- an assertion
 * is usually written about a pointer, and asking for that as an affine
 * expression is what used to end the scop at the assertion, and with it
 * the loop the assertion stands in.
 *
 * What is left unsaid is that the scop describes the executions that do
 * not trap.  That is the contract the assertion itself states.
 */
__isl_give pet_tree *PetScan::extract(IfStmt *stmt)
{
	pet_expr *pe_cond;
	pet_tree *tree, *tree_else;

	if (!stmt->getElse() && traps(stmt->getThen()))
		return pet_tree_new_block(ctx, 0, 0);

	pe_cond = extract_expr(stmt->getCond());
	tree = extract(stmt->getThen());
	if (stmt->getElse()) {
		tree_else = extract(stmt->getElse());
		if (options->autodetect) {
			if (tree && !tree_else) {
				partial = true;
				pet_expr_free(pe_cond);
				return tree;
			}
			if (!tree && tree_else) {
				partial = true;
				pet_expr_free(pe_cond);
				return tree_else;
			}
		}
		tree = pet_tree_new_if_else(pe_cond, tree, tree_else);
	} else
		tree = pet_tree_new_if(pe_cond, tree);
	return tree;
}

/* Is "parent" a compound statement that has "stmt" as its final child?
 */
static bool final_in_compound(ReturnStmt *stmt, Stmt *parent)
{
	CompoundStmt *c;

	c = dyn_cast<CompoundStmt>(parent);
	if (c) {
		StmtIterator i;
		Stmt *last;
		StmtRange range = c->children();

		for (i = range.first; i != range.second; ++i)
			last = *i;
		return last == stmt;
	}
	return false;
}

/* Try and construct a pet_tree for a return statement "stmt".
 *
 * Return statements are only allowed in a context where
 * this->return_root has been set.
 * Furthermore, "stmt" should appear as the last child
 * in the compound statement this->return_root.
 *
 * A return with nothing to return computes nothing.  It is the last
 * statement of the body, so there is nothing after it to leave out
 * either, and what it stands for in a scop is an empty block.  Nothing
 * is collected from it either: there is no expression to look in.
 *
 * What is returned is looked in for calls whose bodies can be put in
 * place, exactly as the expression of an expression statement is --
 * "return f(x)" is how a C interface is written, and a call left as a
 * call there leaves the body of everything behind the interface outside
 * the scop.  Unlike an expression statement, the outermost call is not
 * dropped when its body is put in place: what the return returns is then
 * the variable that body wrote, which is an access like any other.
 */
__isl_give pet_tree *PetScan::extract(ReturnStmt *stmt)
{
	pet_expr *val;
	pet_tree *tree;
	Expr *ret;

	if (!return_root) {
		report_unsupported_return(stmt);
		return NULL;
	}
	if (!final_in_compound(stmt, return_root)) {
		report_return_not_at_end_of_function(stmt);
		return NULL;
	}

	ret = stmt->getRetValue();
	if (!ret)
		return pet_tree_new_block(ctx, 0, 0);

	pet_inlined_calls ic(this);

	ic.collect(ret);
	call2id = &ic.call2id;
	val = extract_expr(ret);
	call2id = NULL;
	tree = pet_tree_new_return(val);
	return ic.add_inlined(tree);
}

/* Try and construct a pet_tree for a label statement.
 */
__isl_give pet_tree *PetScan::extract(LabelStmt *stmt)
{
	isl_id *label;
	pet_tree *tree;

	label = isl_id_alloc(ctx, stmt->getName(), NULL);

	tree = extract(stmt->getSubStmt());
	tree = pet_tree_set_label(tree, label);
	return tree;
}

/* Update the location of "tree" to include the source range of "stmt".
 *
 * Actually, we create a new location based on the source range of "stmt" and
 * then extend this new location to include the region of the original location.
 * This ensures that the line number of the final location refers to "stmt".
 */
__isl_give pet_tree *PetScan::update_loc(__isl_take pet_tree *tree, Stmt *stmt)
{
	pet_loc *loc, *tree_loc;

	tree_loc = pet_tree_get_loc(tree);
	loc = construct_pet_loc(stmt->getSourceRange(), false);
	loc = pet_loc_update_start_end_from_loc(loc, tree_loc);
	pet_loc_free(tree_loc);

	tree = pet_tree_set_loc(tree, loc);
	return tree;
}

/* Is "expr" of a type that can be converted to an access expression?
 *
 * "this" is one: a method reaches its members through it, and what it
 * names is read exactly as a pointer parameter is read.
 */
static bool is_access_expr_type(Expr *expr)
{
	switch (expr->getStmtClass()) {
	case Stmt::ArraySubscriptExprClass:
	case Stmt::DeclRefExprClass:
	case Stmt::MemberExprClass:
	case Stmt::CXXThisExprClass:
		return true;
	default:
		return false;
	}
}

/* Tell the pet_inliner "inliner" about the formal arguments
 * in "fd" and the corresponding actual arguments in "call".
 * Return 0 if this was successful and -1 otherwise.
 *
 * Any pointer argument is treated as an array.
 * The other arguments are treated as scalars.
 *
 * In case of scalars, there is no restriction on the actual argument.
 * This actual argument is assigned to a variable with a name
 * that is derived from the name of the corresponding formal argument,
 * but made not to conflict with any variable names that are
 * already in use.
 *
 * In case of arrays, the actual argument needs to be an expression
 * of a type that can be converted to an access expression or the address
 * of such an expression, ignoring implicit and redundant casts.
 *
 * A call is not required to carry an argument for every parameter the
 * body declares: a parameter with a default value that was never written
 * out is one the call does not hand over, and after a link the call and
 * the body it is matched to were read separately.  Reaching for an
 * argument that is not there reads past the end of the call.  A body
 * whose parameters cannot all be bound is not put in place -- binding
 * some of them would leave the rest standing for nothing -- and the call
 * stays a call, which is what happens to every other argument this
 * cannot make sense of.
 */
int PetScan::set_inliner_arguments(pet_inliner &inliner, CallExpr *call,
	FunctionDecl *fd)
{
	unsigned n;

	n = fd->getNumParams();
	if (n > call->getNumArgs()) {
		report_unsupported_inline_function_argument(call);
		return -1;
	}
	for (unsigned i = 0; i < n; ++i) {
		ParmVarDecl *parm = fd->getParamDecl(i);
		QualType type = parm->getType();
		Expr *arg, *sub;
		pet_expr *expr;
		int is_addr = 0;

		arg = call->getArg(i);
		if (pet_clang_array_depth(type) == 0) {
			string name = parm->getNameAsString();
			if (name_in_use(name, NULL))
				name = generate_new_name(name);
			used_names.insert(name);
			inliner.add_scalar_arg(parm, name, extract_expr(arg));
			continue;
		}
		arg = pet_clang_strip_casts(arg);
		sub = subscript_from_pointer_offset(ast_context, arg);
		if (sub) {
			is_addr = 1;
			arg = sub;
		} else if ((sub = extract_addr_of_arg(arg))) {
			is_addr = 1;
			arg = pet_clang_strip_casts(sub);
		}
		if (!is_access_expr_type(arg)) {
			report_unsupported_inline_function_argument(arg);
			return -1;
		}
		expr = extract_access_expr(arg);
		if (!expr)
			return -1;
		inliner.add_array_arg(parm, expr, is_addr);
	}

	/* The object the method was called on is an argument as well, and
	 * the one the body reaches its members through.  Binding it here
	 * is what keeps the body talking about the caller's object rather
	 * than about an object of its own: without it a write through
	 * this would be a write to something nothing else can see.
	 *
	 * Written "p->m()" the object arrives as a pointer and stands for
	 * this as it is; written "b.m()" it arrives as the object, and
	 * this is its address.
	 */
	CXXMemberCallExpr *member_call = dyn_cast<CXXMemberCallExpr>(call);
	CXXMethodDecl *method = dyn_cast<CXXMethodDecl>(fd);

	if (member_call && method && !method->isStatic()) {
		Expr *object = pet_clang_strip_casts(
					member_call->getImplicitObjectArgument());
		int is_addr = !object->getType()->isPointerType();
		pet_expr *expr;

		if (!is_access_expr_type(object)) {
			report_unsupported_inline_function_argument(object);
			return -1;
		}
		expr = extract_access_expr(object);
		if (!expr)
			return -1;
		inliner.add_array_arg(implicit_object(ast_context, method),
					expr, is_addr);
	}

	return 0;
}

/* Internal data structure for PetScan::substitute_array_sizes.
 * ps is the PetScan on which the method was called.
 * substituter is the substituter that is used to substitute variables
 * in the size expressions.
 */
struct pet_substitute_array_sizes_data {
	PetScan *ps;
	pet_substituter *substituter;
};

extern "C" {
	static int substitute_array_size(__isl_keep pet_tree *tree, void *user);
}

/* If "tree" is a declaration, then perform the substitutions
 * in data->substituter on its size expression and store the result
 * in the size expression cache of data->ps such that the modified expression
 * will be used in subsequent calls to get_array_size.
 */
static int substitute_array_size(__isl_keep pet_tree *tree, void *user)
{
	struct pet_substitute_array_sizes_data *data;
	isl_id *id;
	pet_expr *var, *size;

	if (!pet_tree_is_decl(tree))
		return 0;

	data = (struct pet_substitute_array_sizes_data *) user;
	var = pet_tree_decl_get_var(tree);
	id = pet_expr_access_get_id(var);
	pet_expr_free(var);

	size = data->ps->get_array_size(id);
	size = data->substituter->substitute(size);
	data->ps->set_array_size(id, size);

	return 0;
}

/* Perform the substitutions in "substituter" on all the arrays declared
 * inside "tree" and store the results in the size expression cache
 * such that the modified expressions will be used in subsequent calls
 * to get_array_size.
 */
int PetScan::substitute_array_sizes(__isl_keep pet_tree *tree,
	pet_substituter *substituter)
{
	struct pet_substitute_array_sizes_data data = { this, substituter };

	return pet_tree_foreach_sub_tree(tree, &substitute_array_size, &data);
}

/* Try and construct a pet_tree from the body of "fd" using the actual
 * arguments in "call" in place of the formal arguments.
 * "fd" is assumed to point to the declaration with a function body.
 * In particular, construct a block that consists of assignments
 * of (parts of) the actual arguments to temporary variables
 * followed by the inlined function body with the formal arguments
 * replaced by (expressions containing) these temporary variables.
 * If "return_id" is set, then it is used to store the return value
 * of the inlined function.
 *
 * The actual inlining is taken care of by the pet_inliner object.
 * This function merely calls set_inliner_arguments to tell
 * the pet_inliner about the actual arguments, extracts a pet_tree
 * from the body of the called function and then passes this pet_tree
 * to the pet_inliner.
 * The body of the called function is allowed to have a return statement
 * at the end.
 * The substitutions performed by the inliner are also applied
 * to the size expressions of the arrays declared in the inlined
 * function.  These size expressions are not stored in the tree
 * itself, but rather in the size expression cache.
 *
 * During the extraction of the function body, all variables names
 * that are declared in the calling function as well all variable
 * names that are known to be in use are considered to be in use
 * in the called function to ensure that there is no naming conflict.
 * Similarly, the additional names that are in use in the called function
 * are considered to be in use in the calling function as well.
 *
 * The location of the pet_tree is reset to the call site to ensure
 * that the extent of the scop does not include the body of the called
 * function.
 */
__isl_give pet_tree *PetScan::extract_inlined_call(CallExpr *call,
	FunctionDecl *fd, __isl_keep isl_id *return_id)
{
	pet_tree *tree;
	pet_loc *tree_loc;
	pet_inliner inliner(ctx, walk->n_arg, ast_context);

	if (set_inliner_arguments(inliner, call, fd) < 0)
		return NULL;

	walk->inlining.insert(fd);
	++walk->inline_depth;
	walk->total_stmts += count_body_stmts(fd);

	/* Going over one function can put many bodies in place, and
	 * watching a file that does not grow is no way to learn that.
	 */
	if (walk->total_stmts % 10000 == 0)
		fprintf(stderr, "put %d statement nodes in place so far\n",
			walk->total_stmts);

	PetScan body_scan(PP, ast_context, fd, loc, options,
				isl_union_map_copy(value_bounds), independent);
	body_scan.walk = walk;
	collect_declared_names();
	body_scan.add_new_used_names(declared_names);
	body_scan.add_new_used_names(used_names);
	body_scan.return_root = fd->getBody();
	tree = body_scan.extract(fd->getBody(), false);
	add_new_used_names(body_scan.used_names);

	--walk->inline_depth;
	walk->inlining.erase(fd);

	/* A body that did not come over whole is not put in place of the
	 * call.  Standing where it is, the call is one statement of a
	 * scop and asks nothing of what it calls; put in place of it, a
	 * body that a scop cannot hold takes the call's own statement
	 * down with it, and a function that had a scop before has none.
	 * A union widens, and this has to widen too.
	 *
	 * Why it did not come over is said here, since it is the reason
	 * the scop stopped growing and there is nowhere else it would be
	 * said: what the body's own scan found is about the body.
	 */
	if (getenv("PET_INLINE_TRACE"))
		fprintf(stderr, "the body of %s came back %s, the scan calls "
			"it %s, %zu still being put in place, and the caller "
			"is %s\n", fd->getNameAsString().c_str(),
			tree ? "as a tree" : "as nothing",
			body_scan.partial ? "partial" : "whole",
			walk->inlining.size(),
			partial ? "partial" : "whole");

	if (body_scan.partial || !tree) {
		std::string why = body_scan.first_stop.empty() ?
			std::string("nothing said why") :
			body_scan.first_stop;

		std::string said = "the body of " + fd->getNameAsString() +
					" did not come over whole: " + why;

		/* Both, because this is where the scop stopped growing and
		 * also the last thing that happened to it: a function whose
		 * scop came out wants the first, and one whose scop did not
		 * wants the last, and this is each of them.
		 */
		if (first_stop.empty())
			first_stop = said;
		last_stop = said;
		pet_tree_free(tree);
		return NULL;
	}

	tree_loc = construct_pet_loc(call->getSourceRange(), true);
	tree = pet_tree_set_loc(tree, tree_loc);

	substitute_array_sizes(tree, &inliner);

	return inliner.inline_tree(tree, return_id);
}

/* Is "stmt" a call that can touch nothing the scop describes?
 *
 * A call that is given only values -- numbers, and the addresses of
 * things written in the source, which is what a format and __func__
 * are -- has been handed no way of reaching the memory the scop is
 * about.  Whatever else it does happens outside: it prints.
 *
 * Such a call is left out of the scop entirely, which is a decision and
 * not a reading: the model stops saying that the printing happened, and
 * in exchange it says what the loop around it computes.  Written the
 * other way round, one printf costs the whole loop it stands in.
 *
 * A pointer among the arguments is what tells the two apart.  snprintf
 * is given somewhere to write and is not this; neither is memcpy, nor
 * anything else that was handed an address.
 */
static bool is_printing_call(Stmt *stmt)
{
	CallExpr *call = dyn_cast<CallExpr>(stmt);
	FunctionDecl *fd;

	if (!call)
		return false;
	fd = pet_clang_direct_callee(call);
	if (!fd || !fd->isVariadic())
		return false;
	if (!call->getType()->isVoidType() &&
	    !call->getType()->isIntegerType())
		return false;

	for (unsigned i = 0; i < call->getNumArgs(); ++i) {
		Expr *arg = pet_clang_strip_casts(call->getArg(i));
		QualType qt = arg->getType();

		if (isa<StringLiteral>(arg) || isa<PredefinedExpr>(arg))
			continue;
		if (qt->isPointerType() || qt->isArrayType() ||
		    qt->isReferenceType())
			return false;
	}

	return true;
}

/* Try and construct a pet_tree corresponding
 * to the expression statement "stmt".
 *
 * First look for function calls that have corresponding bodies
 * marked "inline".  Extract the inlined functions in a pet_inlined_calls
 * object.  Then extract the statement itself, replacing calls
 * to inlined function by accesses to the corresponding return variables, and
 * return the combined result.
 * If the outer expression is itself a call to an inlined function,
 * then it already appears as one of the inlined functions and
 * no separate pet_tree needs to be extracted for "stmt" itself.
 */
__isl_give pet_tree *PetScan::extract_expr_stmt(Stmt *stmt)
{
	pet_expr *expr;
	pet_tree *tree;

	if (is_printing_call(stmt))
		return pet_tree_new_block(ctx, 0, 0);

	pet_inlined_calls ic(this);

	ic.collect(stmt);
	/* A statement that is nothing but a call to a function whose body
	 * was put in place needs nothing extracted for it: the body is
	 * among what will be put around it.  A call whose body did not
	 * come over is still a call, and extracting nothing for it would
	 * leave the statement out of the scop altogether -- which is how
	 * a function calling one that a scop cannot hold came to hold
	 * nothing at all, rather than the call it used to hold.
	 */
	if (ic.calls.size() >= 1 && ic.calls[0] == stmt &&
	    ic.done.count(stmt)) {
		tree = pet_tree_new_block(ctx, 0, 0);
	} else {
		call2id = &ic.call2id;
		expr = extract_expr(cast<Expr>(stmt));
		tree = extract(expr, stmt->getSourceRange(), true);
		call2id = NULL;
	}
	tree = ic.add_inlined(tree);
	return tree;
}

/* Try and construct a pet_tree corresponding to "stmt".
 *
 * If "stmt" is a compound statement, then "skip_declarations"
 * indicates whether we should skip initial declarations in the
 * compound statement.
 *
 * If the constructed pet_tree is not a (possibly) partial representation
 * of "stmt", we update start and end of the pet_scop to those of "stmt".
 * In particular, if skip_declarations is set, then we may have skipped
 * declarations inside "stmt" and so the pet_scop may not represent
 * the entire "stmt".
 * Note that this function may be called with "stmt" referring to the entire
 * body of the function, including the outer braces.  In such cases,
 * skip_declarations will be set and the braces will not be taken into
 * account in tree->loc.
 */
__isl_give pet_tree *PetScan::extract(Stmt *stmt, bool skip_declarations)
{
	pet_tree *tree;

	set_current_stmt(stmt);

	if (is_trap_call(stmt))
		return pet_tree_new_block(ctx, 0, 0);

	if (isa<Expr>(stmt))
		return extract_expr_stmt(cast<Expr>(stmt));

	switch (stmt->getStmtClass()) {
	case Stmt::WhileStmtClass:
		tree = extract(cast<WhileStmt>(stmt));
		break;
	case Stmt::ForStmtClass:
		tree = extract_for(cast<ForStmt>(stmt));
		break;
	case Stmt::IfStmtClass:
		tree = extract(cast<IfStmt>(stmt));
		break;
	case Stmt::CompoundStmtClass:
		tree = extract(cast<CompoundStmt>(stmt), skip_declarations);
		break;
	case Stmt::LabelStmtClass:
		tree = extract(cast<LabelStmt>(stmt));
		break;
	case Stmt::ContinueStmtClass:
		tree = pet_tree_new_continue(ctx);
		break;
	case Stmt::BreakStmtClass:
		tree = pet_tree_new_break(ctx);
		break;
	case Stmt::DeclStmtClass:
		tree = extract(cast<DeclStmt>(stmt));
		break;
	case Stmt::NullStmtClass:
		tree = pet_tree_new_block(ctx, 0, 0);
		break;
	case Stmt::ReturnStmtClass:
		tree = extract(cast<ReturnStmt>(stmt));
		break;
	default:
		report_unsupported_statement_type(stmt);
		return NULL;
	}

	if (partial || skip_declarations)
		return tree;

	return update_loc(tree, stmt);
}

/* Given a sequence of statements "stmt_range" of which the first "n_decl"
 * are declarations and of which the remaining statements are represented
 * by "tree", try and extend "tree" to include the last sequence of
 * the initial declarations that can be completely extracted.
 *
 * We start collecting the initial declarations and start over
 * whenever we come across a declaration that we cannot extract.
 * If we have been able to extract any declarations, then we
 * copy over the contents of "tree" at the end of the declarations.
 * Otherwise, we simply return the original "tree".
 */
__isl_give pet_tree *PetScan::insert_initial_declarations(
	__isl_take pet_tree *tree, int n_decl, StmtRange stmt_range)
{
	StmtIterator i;
	pet_tree *res;
	int n_stmt;
	int is_block;
	int j;

	n_stmt = pet_tree_block_n_child(tree);
	is_block = pet_tree_block_get_block(tree);
	res = pet_tree_new_block(ctx, is_block, n_decl + n_stmt);

	for (i = stmt_range.first; n_decl; ++i, --n_decl) {
		Stmt *child = *i;
		pet_tree *tree_i;

		tree_i = extract(child);
		if (tree_i && !partial) {
			res = pet_tree_block_add_child(res, tree_i);
			continue;
		}
		pet_tree_free(tree_i);
		partial = false;
		if (pet_tree_block_n_child(res) == 0)
			continue;
		pet_tree_free(res);
		res = pet_tree_new_block(ctx, is_block, n_decl + n_stmt);
	}

	if (pet_tree_block_n_child(res) == 0) {
		pet_tree_free(res);
		return tree;
	}

	for (j = 0; j < n_stmt; ++j) {
		pet_tree *tree_i;

		tree_i = pet_tree_block_get_child(tree, j);
		res = pet_tree_block_add_child(res, tree_i);
	}
	pet_tree_free(tree);

	return res;
}

/* Try and construct a pet_tree corresponding to (part of)
 * a sequence of statements.
 *
 * "block" is set if the sequence represents the children of
 * a compound statement.
 * "skip_declarations" is set if we should skip initial declarations
 * in the sequence of statements.
 * "parent" is the statement that has stmt_range as (some of) its children.
 *
 * If autodetect is set, then we allow the extraction of only a subrange
 * of the sequence of statements.  However, if there is at least one
 * kill and there is some subsequent statement for which we could not
 * construct a tree, then turn off the "block" property of the tree
 * such that no extra kill will be introduced at the end of the (partial)
 * block.  If, on the other hand, the final range contains
 * no statements, then we discard the entire range.
 * If only a subrange of the sequence was extracted, but each statement
 * in the sequence was extracted completely, and if there are some
 * variable declarations in the sequence before or inside
 * the extracted subrange, then check if any of these variables are
 * not used after the extracted subrange.  If so, add kills to these
 * variables.
 *
 * If the entire range was extracted, apart from some initial declarations,
 * then we try and extend the range with the latest of those initial
 * declarations.
 */
__isl_give pet_tree *PetScan::extract(StmtRange stmt_range, bool block,
	bool skip_declarations, Stmt *parent)
{
	StmtIterator i;
	int j, skip;
	bool has_kills = false;
	bool partial_range = false;
	bool outer_partial = false;
	pet_tree *tree;
	SourceManager &SM = PP.getSourceManager();
	pet_killed_locals kl(SM);
	unsigned range_start, range_end;

	for (i = stmt_range.first, j = 0; i != stmt_range.second; ++i, ++j)
		;

	tree = pet_tree_new_block(ctx, block, j);

	skip = 0;
	i = stmt_range.first;
	if (skip_declarations)
		for (; i != stmt_range.second; ++i) {
			if ((*i)->getStmtClass() != Stmt::DeclStmtClass)
				break;
			if (options->autodetect)
				kl.add_locals(cast<DeclStmt>(*i));
			++skip;
		}

	for (; i != stmt_range.second; ++i) {
		Stmt *child = *i;
		pet_tree *tree_i;

		tree_i = extract(child);
		if (pet_tree_block_n_child(tree) != 0 && partial) {
			pet_tree_free(tree_i);
			break;
		}

		/* A child that holds nothing is not part of the block.
		 *
		 * An empty statement is one such, and so is a trap, which
		 * is where this matters: autodetect takes the first run
		 * of statements it can read and stops at the first it
		 * cannot, skipping over what comes before the run begins.
		 * A child that holds nothing would begin the run without
		 * adding anything to it, so a body that opens with an
		 * assertion would have its run begin at the assertion and
		 * end at the first declaration after it -- 46 statements
		 * of ggml_compute_forward_clamp_f16 became none that way.
		 */
		if (tree_i && pet_tree_get_type(tree_i) == pet_tree_block &&
		    pet_tree_block_n_child(tree_i) == 0) {
			pet_tree_free(tree_i);
			continue;
		}

		if (child->getStmtClass() == Stmt::DeclStmtClass) {
			if (options->autodetect)
				kl.add_locals(cast<DeclStmt>(child));
			if (tree_i && block)
				has_kills = true;
		}
		if (options->autodetect) {
			if (tree_i) {
				range_end = getExpansionOffset(SM,
							end_loc(child));
				if (pet_tree_block_n_child(tree) == 0)
					range_start = getExpansionOffset(SM,
							begin_loc(child));
				tree = pet_tree_block_add_child(tree, tree_i);
			} else {
				partial_range = true;
			}
			if (pet_tree_block_n_child(tree) != 0 && !tree_i)
				outer_partial = partial = true;
		} else {
			tree = pet_tree_block_add_child(tree, tree_i);
		}

		if (partial || !tree)
			break;
	}

	if (!tree)
		return NULL;

	if (partial) {
		if (has_kills)
			tree = pet_tree_block_set_block(tree, 0);
		if (outer_partial) {
			kl.remove_accessed_after(parent,
						 range_start, range_end);
			tree = add_kills(tree, kl.locals);
		}
	} else if (partial_range) {
		if (pet_tree_block_n_child(tree) == 0) {
			pet_tree_free(tree);
			return NULL;
		}
		partial = true;
	} else if (skip > 0)
		tree = insert_initial_declarations(tree, skip, stmt_range);

	return tree;
}

extern "C" {
	static __isl_give pet_expr *get_array_size(__isl_keep pet_expr *access,
		void *user);
	static struct pet_array *extract_array(__isl_keep pet_expr *access,
		__isl_keep pet_context *pc, void *user);
}

/* Construct a pet_expr that holds the sizes of the array accessed
 * by "access".
 * This function is used as a callback to pet_context_add_parameters,
 * which is also passed a pointer to the PetScan object.
 */
static __isl_give pet_expr *get_array_size(__isl_keep pet_expr *access,
	void *user)
{
	PetScan *ps = (PetScan *) user;
	isl_id *id;
	pet_expr *size;

	id = pet_expr_access_get_id(access);
	size = ps->get_array_size(id);
	isl_id_free(id);

	return size;
}

/* Construct and return a pet_array corresponding to the variable
 * accessed by "access".
 * This function is used as a callback to pet_scop_from_pet_tree,
 * which is also passed a pointer to the PetScan object.
 */
static struct pet_array *extract_array(__isl_keep pet_expr *access,
	__isl_keep pet_context *pc, void *user)
{
	PetScan *ps = (PetScan *) user;
	isl_id *id;
	pet_array *array;

	id = pet_expr_access_get_id(access);
	array = ps->extract_array(id, NULL, pc);
	isl_id_free(id);

	return array;
}

/* Store (a copy of) "summary" in the cache of function summaries
 * for function declaration "fd" and then give it back to the caller.
 */
__isl_give pet_function_summary *PetScan::cache_summary(clang::FunctionDecl *fd,
	__isl_take pet_function_summary *summary)
{
	summary_cache[fd] = pet_function_summary_copy(summary);

	return summary;
}

/* Extract a function summary from the body of "fd",
 * with pet_tree representation "tree", extracted using "body_scan".
 *
 * We extract a scop from the function body in a context with as
 * parameters the integer arguments of the function.
 * We then collect the accessed array elements and attach them
 * to the corresponding array arguments, taking into account
 * that the function body may access members of array elements.
 *
 * The reason for representing the integer arguments as parameters in
 * the context is that if we were to instead start with a context
 * with the function arguments as initial dimensions, then we would not
 * be able to refer to them from the array extents, without turning
 * array extents into maps.
 */
__isl_give pet_function_summary *PetScan::get_summary_from_tree(
	__isl_take pet_tree *tree, clang::FunctionDecl *fd,
	PetScan &body_scan)
{
	isl_space *space;
	isl_set *domain;
	pet_context *pc;
	pet_function_summary *summary;
	unsigned n;
	struct pet_scop *scop;
	int int_size;
	isl_union_set *may_read, *may_write, *must_write;
	isl_union_map *to_inner;

	space = isl_space_set_alloc(ctx, 0, 0);

	n = fd->getNumParams();
	summary = pet_function_summary_alloc(ctx, n);
	for (unsigned i = 0; i < n; ++i) {
		ParmVarDecl *parm = fd->getParamDecl(i);
		QualType type = parm->getType();
		isl_id *id;

		if (!type->isIntegerType())
			continue;
		id = pet_id_from_decl(ctx, parm);
		space = isl_space_insert_dims(space, isl_dim_param, 0, 1);
		space = isl_space_set_dim_id(space, isl_dim_param, 0,
						isl_id_copy(id));
		summary = pet_function_summary_set_int(summary, i, id);
	}

	domain = isl_set_universe(space);
	pc = pet_context_alloc(domain);
	pc = pet_context_add_parameters(pc, tree,
						&::get_array_size, &body_scan);
	int_size = size_in_bytes(ast_context, ast_context.IntTy);
	scop = pet_scop_from_pet_tree(tree, int_size,
					&::extract_array, &body_scan, pc);
	/* NOT the scop the annotation is about.  This one is a called
	 * function's body, built only to compute its summary and freed
	 * below; pet_arena_map is file-scope and still holds the caller's
	 * pragmas, so adding their arrays here invents twelve arrays in a
	 * scop that mentions none of them -- and reports all twelve as
	 * unspelled, which is what it did.
	 */
	scop = scan_arrays(scop, pc, 0);
	may_read = isl_union_map_range(pet_scop_get_may_reads(scop));
	may_write = isl_union_map_range(pet_scop_get_may_writes(scop));
	must_write = isl_union_map_range(pet_scop_get_must_writes(scop));
	to_inner = pet_scop_compute_outer_to_inner(scop);
	pet_scop_free(scop);

	for (unsigned i = 0; i < n; ++i) {
		ParmVarDecl *parm = fd->getParamDecl(i);
		QualType type = parm->getType();
		struct pet_array *array;
		isl_space *space;
		isl_union_set *data_set;
		isl_union_set *may_read_i, *may_write_i, *must_write_i;

		if (pet_clang_array_depth(type) == 0)
			continue;

		array = body_scan.extract_array(parm, NULL, pc);
		space = array ? isl_set_get_space(array->extent) : NULL;
		pet_array_free(array);
		data_set = isl_union_set_from_set(isl_set_universe(space));
		data_set = isl_union_set_apply(data_set,
					isl_union_map_copy(to_inner));
		may_read_i = isl_union_set_intersect(
				isl_union_set_copy(may_read),
				isl_union_set_copy(data_set));
		may_write_i = isl_union_set_intersect(
				isl_union_set_copy(may_write),
				isl_union_set_copy(data_set));
		must_write_i = isl_union_set_intersect(
				isl_union_set_copy(must_write), data_set);
		summary = pet_function_summary_set_array(summary, i,
				may_read_i, may_write_i, must_write_i);
	}

	isl_union_set_free(may_read);
	isl_union_set_free(may_write);
	isl_union_set_free(must_write);
	isl_union_map_free(to_inner);

	pet_context_free(pc);

	return summary;
}

/* How far a chain of summaries is followed.
 *
 * A summary is worked out from the body of what is called, which is
 * worked out from the bodies of what that calls, and a program gives no
 * reason for that to be shallow.  A cycle is caught for what it is;
 * this is the second line, for a chain that is merely long, and it is
 * set where the chains of a real program do not reach.
 */
static const int max_summary_depth = 64;

/* Extract a function summary from the body of "fd", if possible.
 * Return this->no_summary if the body cannot be fully analyzed.
 *
 * Turn on autodetection to avoid printing warnings
 * if the body cannot be fully analyzed,
 * but return this->no_summary if the extracted pet_tree only
 * represents part of the function body.
 * The function body is allowed to have a return statement at the end.
 *
 * The result is stored in the summary_cache cache so that we can reuse
 * it if this method gets called on the same function again later on.
 */
__isl_give pet_function_summary *PetScan::get_summary(FunctionDecl *fd)
{
	pet_tree *tree;
	pet_function_summary *summary;
	ScopLoc loc;
	int save_autodetect;

	if (summary_cache.find(fd) != summary_cache.end())
		return pet_function_summary_copy(summary_cache[fd]);

	/* A function whose summary is already being worked out is one this
	 * call came from, and asking again is asking the question that has
	 * not been answered yet.  It is left without a summary, which is
	 * what a call to something nothing is known about gets anyway, and
	 * the answer is not remembered: it is true of this way round and
	 * not of the function.
	 *
	 * Two functions of an engine that call each other went round this
	 * sixteen thousand times before the stack ran out, whatever the
	 * stack was set to.
	 */
	if (walk->in_summary.find(fd) != walk->in_summary.end() ||
	    walk->summary_depth >= max_summary_depth) {
		if (getenv("PET_SCOP_TRACE")) {
			fprintf(stderr, "no summary for %s: it is %s\n",
				fd->getNameAsString().c_str(),
				walk->in_summary.find(fd) !=
					walk->in_summary.end() ?
					"already being worked out" :
					"deeper than summaries are followed");
			for (FunctionDecl *on : walk->in_summary)
				fprintf(stderr, "  by way of %s\n",
					on->getNameAsString().c_str());
		}
		return pet_function_summary_copy(no_summary);
	}

	walk->in_summary.insert(fd);
	++walk->summary_depth;

	save_autodetect = options->autodetect;
	options->autodetect = 1;
	PetScan body_scan(PP, ast_context, fd, loc, options,
				isl_union_map_copy(value_bounds), independent);

	body_scan.walk = walk;
	body_scan.return_root = fd->getBody();
	tree = body_scan.extract(fd->getBody(), false);
	options->autodetect = save_autodetect;

	--walk->summary_depth;
	walk->in_summary.erase(fd);

	if (body_scan.partial) {
		pet_tree_free(tree);
		return cache_summary(fd, pet_function_summary_copy(no_summary));
	}

	summary = get_summary_from_tree(tree, fd, body_scan);

	return cache_summary(fd, summary);
}

/* If "fd" has a function body, then try and extract a function summary from
 * this body and, if successful, attach it to the call expression "expr".
 *
 * Even if a function body is available, "fd" itself may point
 * to a declaration without function body.  We therefore first
 * replace it by the declaration that comes with a body (if any).
 */
__isl_give pet_expr *PetScan::set_summary(__isl_take pet_expr *expr,
	FunctionDecl *fd)
{
	pet_function_summary *summary;

	if (!expr)
		return NULL;
	fd = pet_clang_find_function_decl_with_body(fd);
	if (!fd)
		return expr;

	summary = get_summary(fd);
	if (summary == no_summary) {
		pet_function_summary_free(summary);
		return expr;
	}

	expr = pet_expr_call_set_summary(expr, summary);

	return expr;
}

/* Extract a pet_scop from "tree".
 *
 * We simply call pet_scop_from_pet_tree with the appropriate arguments and
 * then add pet_arrays for all accessed arrays.
 * We populate the pet_context with assignments for all parameters used
 * inside "tree" or any of the size expressions for the arrays accessed
 * by "tree" so that they can be used in affine expressions.
 */
struct pet_scop *PetScan::extract_scop(__isl_take pet_tree *tree)
{
	int int_size;
	isl_set *domain;
	pet_context *pc;
	pet_scop *scop;

	int_size = size_in_bytes(ast_context, ast_context.IntTy);

	domain = isl_set_universe(isl_space_set_alloc(ctx, 0, 0));
	pc = pet_context_alloc(domain);
	pc = pet_context_add_parameters(pc, tree, &::get_array_size, this);
	scop = pet_scop_from_pet_tree(tree, int_size,
					&::extract_array, this, pc);
	scop = scan_arrays(scop, pc, 1);
	pet_context_free(pc);

	return scop;
}

/* Add a call to __pencil_kill to the end of "tree" that kills
 * all the variables in "locals" and return the result.
 *
 * No location is added to the kill because the most natural
 * location would lie outside the scop.  Attaching such a location
 * to this tree would extend the scope of the final result
 * to include the location.
 */
__isl_give pet_tree *PetScan::add_kills(__isl_take pet_tree *tree,
	set<ValueDecl *> locals)
{
	int i;
	pet_expr *expr;
	pet_tree *kill, *block;
	set<ValueDecl *>::iterator it;

	if (locals.size() == 0)
		return tree;
	expr = pet_expr_new_call(ctx, "__pencil_kill", locals.size());
	i = 0;
	for (it = locals.begin(); it != locals.end(); ++it) {
		pet_expr *arg;
		arg = extract_access_expr(*it);
		expr = pet_expr_set_arg(expr, i++, arg);
	}
	kill = pet_tree_new_expr(expr);
	block = pet_tree_new_block(ctx, 0, 2);
	block = pet_tree_block_add_child(block, tree);
	block = pet_tree_block_add_child(block, kill);

	return block;
}

/* Check if the scop marked by the user is exactly this Stmt
 * or part of this Stmt.
 * If so, return a pet_scop corresponding to the marked region.
 * Otherwise, return NULL.
 *
 * If the scop is not further nested inside a child of "stmt",
 * then check if there are any variable declarations before the scop
 * inside "stmt".  If so, and if these variables are not used
 * after the scop, then add kills to the variables.
 *
 * If the scop starts in the middle of one of the children, without
 * also ending in that child, then report an error.
 */
struct pet_scop *PetScan::scan(Stmt *stmt)
{
	SourceManager &SM = PP.getSourceManager();
	unsigned start_off, end_off;
	pet_tree *tree;

	start_off = getExpansionOffset(SM, begin_loc(stmt));
	end_off = getExpansionOffset(SM, end_loc(stmt));

	if (start_off > loc.end)
		return NULL;
	if (end_off < loc.start)
		return NULL;

	if (start_off >= loc.start && end_off <= loc.end)
		return extract_scop(extract(stmt));

	pet_killed_locals kl(SM);
	StmtIterator start;
	for (start = stmt->child_begin(); start != stmt->child_end(); ++start) {
		Stmt *child = *start;
		if (!child)
			continue;
		start_off = getExpansionOffset(SM, begin_loc(child));
		end_off = getExpansionOffset(SM, end_loc(child));
		if (start_off < loc.start && end_off >= loc.end)
			return scan(child);
		if (start_off >= loc.start)
			break;
		if (loc.start < end_off) {
			report_unbalanced_pragmas(loc.scop, loc.endscop);
			return NULL;
		}
		if (isa<DeclStmt>(child))
			kl.add_locals(cast<DeclStmt>(child));
	}

	StmtIterator end;
	for (end = start; end != stmt->child_end(); ++end) {
		Stmt *child = *end;
		start_off = SM.getFileOffset(begin_loc(child));
		if (start_off >= loc.end)
			break;
	}

	kl.remove_accessed_after(stmt, loc.start, loc.end);

	tree = extract(StmtRange(start, end), false, false, stmt);
	tree = add_kills(tree, kl.locals);
	return extract_scop(tree);
}

/* Set the size of index "pos" of "array" to "size".
 * In particular, add a constraint of the form
 *
 *	i_pos < size
 *
 * to array->extent and a constraint of the form
 *
 *	size >= 0
 *
 * to array->context.
 *
 * The domain of "size" is assumed to be zero-dimensional.
 */
static struct pet_array *update_size(struct pet_array *array, int pos,
	__isl_take isl_pw_aff *size)
{
	isl_set *valid;
	isl_set *univ;
	isl_set *bound;
	isl_space *dim;
	isl_aff *aff;
	isl_pw_aff *index;
	isl_id *id;

	if (!array)
		goto error;

	valid = isl_set_params(isl_pw_aff_nonneg_set(isl_pw_aff_copy(size)));
	array->context = isl_set_intersect(array->context, valid);

	dim = isl_set_get_space(array->extent);
	aff = isl_aff_zero_on_domain(isl_local_space_from_space(dim));
	aff = isl_aff_add_coefficient_si(aff, isl_dim_in, pos, 1);
	univ = isl_set_universe(isl_aff_get_domain_space(aff));
	index = isl_pw_aff_alloc(univ, aff);

	size = isl_pw_aff_add_dims(size, isl_dim_in,
				isl_set_dim(array->extent, isl_dim_set));
	id = isl_set_get_tuple_id(array->extent);
	size = isl_pw_aff_set_tuple_id(size, isl_dim_in, id);
	bound = isl_pw_aff_lt_set(index, size);

	array->extent = isl_set_intersect(array->extent, bound);

	if (!array->context || !array->extent)
		return pet_array_free(array);

	return array;
error:
	isl_pw_aff_free(size);
	return NULL;
}

#ifdef HAVE_DECAYEDTYPE

/* If "qt" is a decayed type, then set *decayed to true and
 * return the original type.
 */
static QualType undecay(QualType qt, bool *decayed)
{
	const Type *type = qt.getTypePtr();

	*decayed = isa<DecayedType>(type);
	if (*decayed)
		qt = cast<DecayedType>(type)->getOriginalType();
	return qt;
}

#else

/* If "qt" is a decayed type, then set *decayed to true and
 * return the original type.
 * Since this version of clang does not define a DecayedType,
 * we cannot obtain the original type even if it had been decayed and
 * we set *decayed to false.
 */
static QualType undecay(QualType qt, bool *decayed)
{
	*decayed = false;
	return qt;
}

#endif

/* Figure out the size of the array at position "pos" and all
 * subsequent positions from "qt" and update the corresponding
 * argument of "expr" accordingly.
 *
 * The initial type (when pos is zero) may be a pointer type decayed
 * from an array type, if this initial type is the type of a function
 * argument.  This only happens if the original array type has
 * a constant size in the outer dimension as otherwise we get
 * a VariableArrayType.  Try and obtain this original type (if available) and
 * take the outer array size into account if it was marked static.
 */
__isl_give pet_expr *PetScan::set_upper_bounds(__isl_take pet_expr *expr,
	QualType qt, int pos)
{
	const ArrayType *atype;
	pet_expr *size;
	bool decayed = false;

	if (!expr)
		return NULL;

	if (pos == 0)
		qt = undecay(qt, &decayed);

	if (qt->isPointerType()) {
		qt = qt->getPointeeType();
		return set_upper_bounds(expr, qt, pos + 1);
	}
	if (!qt->isArrayType())
		return expr;

	qt = qt->getCanonicalTypeInternal();
	atype = cast<ArrayType>(qt.getTypePtr());

	if (decayed && atype->getSizeModifier() != ArraySizeModifier::Static) {
		qt = atype->getElementType();
		return set_upper_bounds(expr, qt, pos + 1);
	}

	if (qt->isConstantArrayType()) {
		const ConstantArrayType *ca = cast<ConstantArrayType>(atype);
		size = extract_expr(ca->getSize());
		expr = pet_expr_set_arg(expr, pos, size);
	} else if (qt->isVariableArrayType()) {
		const VariableArrayType *vla = cast<VariableArrayType>(atype);
		size = extract_expr(vla->getSizeExpr());
		expr = pet_expr_set_arg(expr, pos, size);
	}

	qt = atype->getElementType();

	return set_upper_bounds(expr, qt, pos + 1);
}

/* Construct a pet_expr that holds the sizes of the array represented by "id".
 * The returned expression is a call expression with as arguments
 * the sizes in each dimension.  If we are unable to derive the size
 * in a given dimension, then the corresponding argument is set to infinity.
 * In fact, we initialize all arguments to infinity and then update
 * them if we are able to figure out the size.
 *
 * The result is stored in the id_size cache so that it can be reused
 * if this method is called on the same array identifier later.
 * The result is also stored in the type_size cache in case
 * it gets called on a different array identifier with the same type.
 */
__isl_give pet_expr *PetScan::get_array_size(__isl_keep isl_id *id)
{
	QualType qt = pet_id_get_array_type(id);
	int depth;
	pet_expr *expr, *inf;
	const Type *type = qt.getTypePtr();
	isl_maybe_pet_expr m;

	m = isl_id_to_pet_expr_try_get(id_size, id);
	if (m.valid < 0 || m.valid)
		return m.value;
	if (type_size.find(type) != type_size.end())
		return pet_expr_copy(type_size[type]);

	depth = pet_clang_array_depth(qt);
	inf = pet_expr_new_int(isl_val_infty(ctx));
	expr = pet_expr_new_call(ctx, "bounds", depth);
	for (int i = 0; i < depth; ++i)
		expr = pet_expr_set_arg(expr, i, pet_expr_copy(inf));
	pet_expr_free(inf);

	expr = set_upper_bounds(expr, qt, 0);
	type_size[type] = pet_expr_copy(expr);
	id_size = isl_id_to_pet_expr_set(id_size, isl_id_copy(id),
					pet_expr_copy(expr));

	return expr;
}

/* Set the array size of the array identified by "id" to "size",
 * replacing any previously stored value.
 */
void PetScan::set_array_size(__isl_take isl_id *id, __isl_take pet_expr *size)
{
	id_size = isl_id_to_pet_expr_set(id_size, id, size);
}

/* Does "expr" represent the "integer" infinity?
 */
static int is_infty(__isl_keep pet_expr *expr)
{
	isl_val *v;
	int res;

	if (pet_expr_get_type(expr) != pet_expr_int)
		return 0;
	v = pet_expr_int_get_val(expr);
	res = isl_val_is_infty(v);
	isl_val_free(v);

	return res;
}

/* Figure out the dimensions of an array "array" and
 * update "array" accordingly.
 *
 * We first construct a pet_expr that holds the sizes of the array
 * in each dimension.  The resulting expression may containing
 * infinity values for dimension where we are unable to derive
 * a size expression.
 *
 * The arguments of the size expression that have a value different from
 * infinity are then converted to an affine expression
 * within the context "pc" and incorporated into the size of "array".
 * If we are unable to convert a size expression to an affine expression or
 * if the size is not a (symbolic) constant,
 * then we leave the corresponding size of "array" untouched.
 */
struct pet_array *PetScan::set_upper_bounds(struct pet_array *array,
	__isl_keep pet_context *pc)
{
	int n;
	isl_id *id;
	pet_expr *expr;

	if (!array)
		return NULL;

	id = isl_set_get_tuple_id(array->extent);
	if (!id)
		return pet_array_free(array);
	expr = get_array_size(id);
	isl_id_free(id);

	n = pet_expr_get_n_arg(expr);
	for (int i = 0; i < n; ++i) {
		pet_expr *arg;
		isl_pw_aff *size;

		arg = pet_expr_get_arg(expr, i);
		if (!is_infty(arg)) {
			int dim;

			size = pet_expr_extract_affine(arg, pc);
			dim = isl_pw_aff_dim(size, isl_dim_in);
			if (!size)
				array = pet_array_free(array);
			else if (isl_pw_aff_involves_nan(size) ||
			    isl_pw_aff_involves_dims(size, isl_dim_in, 0, dim))
				isl_pw_aff_free(size);
			else {
				size = isl_pw_aff_drop_dims(size,
							    isl_dim_in, 0, dim);
				array = update_size(array, i, size);
			}
		}
		pet_expr_free(arg);
	}
	pet_expr_free(expr);

	return array;
}

/* Does "decl" have a definition that we can keep track of in a pet_type?
 */
static bool has_printable_definition(RecordDecl *decl)
{
	if (!decl->getDeclName())
		return false;
	return decl->getLexicalDeclContext() == decl->getDeclContext();
}

/* The C for the type of a variable whose record type has no name.
 *
 * clang spells such a type "union (unnamed at ggml-impl.h:382:5)", which is a
 * diagnostic and not C: a scop containing
 *
 *	union { float as_value; uint32_t as_bits; } fp32;
 *
 * -- ggml's fp32_to_bits, and every bit pun written the same way -- came out
 * of ppcg with that phrase where the type belonged and did not compile.  The
 * variable is real, the type is real, and only the NAME is missing, because
 * the source never gave it one.  So the definition is written out in place of
 * the name, which is what the source itself says and is valid C wherever a
 * type may appear.
 *
 * Two variables spelled this way get two distinct types, exactly as they do
 * in the source: an unnamed record type is unique to its declaration, so
 * nothing that was assignable becomes unassignable.
 */
static std::string anonymous_record_definition(RecordDecl *decl,
	ASTContext &ast_context)
{
	std::string s;
	llvm::raw_string_ostream S(s);
	PrintingPolicy policy = ast_context.getPrintingPolicy();

	policy.IncludeTagDefinition = 1;
	policy.SuppressTagKeyword = 0;
	decl->print(S, policy, 0, true);
	S.flush();

	return s;
}

/* Add all TypedefType objects that appear when dereferencing "type"
 * to "types".
 */
static void insert_intermediate_typedefs(PetTypes *types, QualType type)
{
	type = pet_clang_base_or_typedef_type(type);
	while (isa<TypedefType>(type)) {
		const TypedefType *tt;

		tt = cast<TypedefType>(type);
		types->insert(tt->getDecl());
		type = tt->desugar();
		type = pet_clang_base_or_typedef_type(type);
	}
}

/* Construct and return a pet_array corresponding to the variable
 * represented by "id".
 * In particular, initialize array->extent to
 *
 *	{ name[i_1,...,i_d] : i_1,...,i_d >= 0 }
 *
 * and then call set_upper_bounds to set the upper bounds on the indices
 * based on the type of the variable.  The upper bounds are converted
 * to affine expressions within the context "pc".
 *
 * If the base type is that of a record with a top-level definition or
 * of a typedef and if "types" is not null, then the RecordDecl or
 * TypedefType corresponding to the type, as well as any intermediate
 * TypedefType, is added to "types".
 *
 * If the base type is that of a record with no top-level definition,
 * then we replace it by "<subfield>".
 *
 * If the variable is a scalar, i.e., a zero-dimensional array,
 * then the "const" qualifier, if any, is removed from the base type.
 * This makes it easier for users of pet to turn initializations
 * into assignments.
 */
struct pet_array *PetScan::extract_array(__isl_keep isl_id *id,
	PetTypes *types, __isl_keep pet_context *pc)
{
	struct pet_array *array;
	QualType qt = pet_id_get_array_type(id);
	int depth = pet_clang_array_depth(qt);
	QualType base = pet_clang_base_type(qt);
	ValueDecl *decl = pet_id_get_decl(id);
	string name;
	isl_space *space;
	long lo;
	int i;

	array = isl_calloc_type(ctx, struct pet_array);
	if (!array)
		return NULL;

	space = isl_space_set_alloc(ctx, 0, depth);
	space = isl_space_set_tuple_id(space, isl_dim_set, isl_id_copy(id));

	/* The outer bound is the annotation's, when one puts this array's
	 * storage below its own start.  See arena_floor.
	 */
	lo = decl ? arena_floor(decl, pointee_element_size(qt, ast_context)) : 0;
	array->extent = isl_set_universe(space);
	for (i = 0; i < depth; ++i)
		array->extent = isl_set_lower_bound_si(array->extent,
						isl_dim_set, i, i ? 0 : lo);

	space = isl_space_params_alloc(ctx, 0);
	array->context = isl_set_universe(space);

	array = set_upper_bounds(array, pc);
	if (!array)
		return NULL;

	if (depth == 0)
		base.removeLocalConst();
	name = base.getAsString();

	/* An unnamed record type is written out, not named.  This is decided
	 * before the "types" block below and independently of it: a scop's
	 * local variables are extracted with no PetTypes at all, and those
	 * are exactly the ones an inlined bit pun brings in.
	 *
	 * A record that is unnamed but typedef'd HAS a name -- the typedef's
	 * -- and keeps it.  Writing the definition out for those made every
	 * ggml_bf16_t in a scop a distinct anonymous struct type, so the
	 * assignment that read one from an array no longer compiled.
	 */
	if (base->isRecordType()) {
		RecordDecl *rd = pet_clang_record_decl(base);

		if (rd && !rd->getDeclName() && !rd->getTypedefNameForAnonDecl() &&
		    rd->isCompleteDefinition())
			name = anonymous_record_definition(rd, ast_context);
	}

	if (types) {
		insert_intermediate_typedefs(types, qt);
		if (isa<TypedefType>(base)) {
			types->insert(cast<TypedefType>(base)->getDecl());
		} else if (base->isRecordType()) {
			RecordDecl *decl = pet_clang_record_decl(base);
			if (!decl)
				return NULL;
			TypedefNameDecl *typedecl;
			typedecl = decl->getTypedefNameForAnonDecl();
			if (typedecl)
				types->insert(typedecl);
			else if (has_printable_definition(decl))
				types->insert(decl);
			else if (decl->getDeclName())
				name = "<subfield>";
		}
	}

	array->element_type = strdup(name.c_str());
	array->element_is_record = base->isRecordType();
	array->element_size = size_in_bytes(ast_context, base);

	return array;
}

/* Construct and return a pet_array corresponding to the variable "decl".
 */
struct pet_array *PetScan::extract_array(ValueDecl *decl,
	PetTypes *types, __isl_keep pet_context *pc)
{
	isl_id *id;
	pet_array *array;

	id = pet_id_from_decl(ctx, decl);
	array = extract_array(id, types, pc);
	isl_id_free(id);

	return array;
}

/* Construct and return a pet_array corresponding to the sequence
 * of declarations represented by "decls".
 * The upper bounds of the array are converted to affine expressions
 * within the context "pc".
 * If the sequence contains a single declaration, then it corresponds
 * to a simple array access.  Otherwise, it corresponds to a member access,
 * with the declaration for the substructure following that of the containing
 * structure in the sequence of declarations.
 * We start with the outermost substructure and then combine it with
 * information from the inner structures.
 *
 * Additionally, keep track of all required types in "types".
 */
struct pet_array *PetScan::extract_array(__isl_keep isl_id_list *decls,
	PetTypes *types, __isl_keep pet_context *pc)
{
	int i, n;
	isl_id *id;
	struct pet_array *array;

	id = isl_id_list_get_id(decls, 0);
	array = extract_array(id, types, pc);
	isl_id_free(id);

	n = isl_id_list_n_id(decls);
	for (i = 1; i < n; ++i) {
		struct pet_array *parent;
		const char *base_name, *field_name;
		char *product_name;

		parent = array;
		id = isl_id_list_get_id(decls, i);
		array = extract_array(id, types, pc);
		isl_id_free(id);
		if (!array)
			return pet_array_free(parent);

		base_name = isl_set_get_tuple_name(parent->extent);
		field_name = isl_set_get_tuple_name(array->extent);
		product_name = pet_array_member_access_name(ctx,
							base_name, field_name);

		array->extent = isl_set_product(isl_set_copy(parent->extent),
						array->extent);
		if (product_name)
			array->extent = isl_set_set_tuple_name(array->extent,
								product_name);
		array->context = isl_set_intersect(array->context,
						isl_set_copy(parent->context));

		pet_array_free(parent);
		free(product_name);

		if (!array->extent || !array->context || !product_name)
			return pet_array_free(array);
	}

	return array;
}

static struct pet_scop *add_type(isl_ctx *ctx, struct pet_scop *scop,
	RecordDecl *decl, Preprocessor &PP, PetTypes &types,
	std::set<TypeDecl *> &types_done, int &n_alloc);
static struct pet_scop *add_type(isl_ctx *ctx, struct pet_scop *scop,
	TypedefNameDecl *decl, Preprocessor &PP, PetTypes &types,
	std::set<TypeDecl *> &types_done, int &n_alloc);

/* For each of the fields of "decl" that is itself a record type
 * or a typedef, or an array of such type, add a corresponding pet_type
 * to "scop".
 */
/* Make room in "scop" for one more type.
 *
 * The types are counted before they are added, but adding one adds the
 * types of its fields as well, and those were not counted: a structure
 * whose field is a structure of its own reaches further than the count
 * of what was named directly.  So the room is made as it is needed.
 */
static struct pet_scop *grow_types(isl_ctx *ctx, struct pet_scop *scop,
	int &n_alloc)
{
	struct pet_type **types;
	int n;

	if (scop->n_type < n_alloc)
		return scop;

	n = n_alloc ? 2 * n_alloc : 4;
	types = isl_realloc_array(ctx, scop->types, struct pet_type *, n);
	if (!types)
		return pet_scop_free(scop);
	scop->types = types;
	n_alloc = n;

	return scop;
}

static struct pet_scop *add_field_types(isl_ctx *ctx, struct pet_scop *scop,
	RecordDecl *decl, Preprocessor &PP, PetTypes &types,
	std::set<TypeDecl *> &types_done, int &n_alloc)
{
	RecordDecl::field_iterator it;

	for (it = decl->field_begin(); it != decl->field_end(); ++it) {
		QualType type = it->getType();

		type = pet_clang_base_or_typedef_type(type);
		if (isa<TypedefType>(type)) {
			TypedefNameDecl *typedefdecl;

			typedefdecl = cast<TypedefType>(type)->getDecl();
			scop = add_type(ctx, scop, typedefdecl,
				PP, types, types_done, n_alloc);
		} else if (type->isRecordType()) {
			RecordDecl *record;

			record = pet_clang_record_decl(type);
			if (record)
				scop = add_type(ctx, scop, record,
					PP, types, types_done, n_alloc);
		}
	}

	return scop;
}

/* Add a pet_type corresponding to "decl" to "scop", provided
 * it is a member of types.records and it has not been added before
 * (i.e., it is not a member of "types_done").
 *
 * Since we want the user to be able to print the types
 * in the order in which they appear in the scop, we need to
 * make sure that types of fields in a structure appear before
 * that structure.  We therefore call ourselves recursively
 * through add_field_types on the types of all record subfields.
 *
 * The declaration is written down as handled before those fields are
 * looked at rather than after they have been added, because one of them
 * may lead back here: a structure holding a pointer to its own kind, or
 * to another that holds one back, would otherwise be descended into for
 * as long as there is stack to do it with.
 */
static struct pet_scop *add_type(isl_ctx *ctx, struct pet_scop *scop,
	RecordDecl *decl, Preprocessor &PP, PetTypes &types,
	std::set<TypeDecl *> &types_done, int &n_alloc)
{
	string s;
	llvm::raw_string_ostream S(s);

	if (types.records.find(decl) == types.records.end())
		return scop;
	if (types_done.find(decl) != types_done.end())
		return scop;
	types_done.insert(decl);

	scop = add_field_types(ctx, scop, decl, PP, types, types_done,
				n_alloc);
	if (!scop)
		return NULL;

	if (decl->getNameAsString().empty())
		return scop;

	decl->print(S, PrintingPolicy(PP.getLangOpts()));
	S.str();

	scop = grow_types(ctx, scop, n_alloc);
	if (!scop)
		return NULL;
	scop->types[scop->n_type] = pet_type_alloc(ctx,
				    decl->getNameAsString().c_str(), s.c_str());
	if (!scop->types[scop->n_type])
		return pet_scop_free(scop);

	scop->n_type++;

	return scop;
}

/* Add a pet_type corresponding to "decl" to "scop", provided
 * it is a member of types.typedefs and it has not been added before
 * (i.e., it is not a member of "types_done").
 *
 * If the underlying type is a structure, then we print the typedef
 * ourselves since clang does not print the definition of the structure
 * in the typedef.  We also make sure in this case that the types of
 * the fields in the structure are added first.
 * Since the definition of the structure also gets printed this way,
 * add it to types_done such that it will not be printed again,
 * not even without the typedef.
 */
static struct pet_scop *add_type(isl_ctx *ctx, struct pet_scop *scop,
	TypedefNameDecl *decl, Preprocessor &PP, PetTypes &types,
	std::set<TypeDecl *> &types_done, int &n_alloc)
{
	string s;
	llvm::raw_string_ostream S(s);
	QualType qt = decl->getUnderlyingType();

	if (types.typedefs.find(decl) == types.typedefs.end())
		return scop;
	if (types_done.find(decl) != types_done.end())
		return scop;
	types_done.insert(decl);

	if (qt->isRecordType()) {
		RecordDecl *rec = pet_clang_record_decl(qt);
		if (!rec)
			return scop;

		scop = add_field_types(ctx, scop, rec, PP, types,
					types_done, n_alloc);
		if (!scop)
			return NULL;
		S << "typedef ";
		rec->print(S, PrintingPolicy(PP.getLangOpts()));
		S << " ";
		S << decl->getNameAsString();
		types_done.insert(rec);
	} else {
		decl->print(S, PrintingPolicy(PP.getLangOpts()));
	}
	S.str();

	scop = grow_types(ctx, scop, n_alloc);
	if (!scop)
		return NULL;
	scop->types[scop->n_type] = pet_type_alloc(ctx,
				    decl->getNameAsString().c_str(), s.c_str());
	if (!scop->types[scop->n_type])
		return pet_scop_free(scop);

	types_done.insert(decl);

	scop->n_type++;

	return scop;
}

/* Add every array an arena annotation names to "arrays", if it is not
 * already there.
 *
 * AN ARRAY THE ANNOTATION NAMES IS AN ARRAY, WHETHER OR NOT ANYONE
 * SUBSCRIPTS IT.  pet_scop_collect_arrays builds the list from ACCESSES,
 * and composition replaces a member's access RELATION while leaving its
 * INDEX naming the member.  So a representative the source never writes
 * as "rep[...]" gets no array -- and then scop_collect_accesses, which
 * intersects every access's range with the extents, removes every access
 * the members composed onto it.  The annotation becomes a silent no-op:
 * the aliasing it was written to declare is invisible again, which is the
 * fault it exists to prevent.
 *
 * Measured on the 402-node scop.  hca_k_all_3_350 is touched only through
 * a second pointer -- "(ggml_fp16_t (*)[128]) hca_k_all_3_350" -- and
 * leaf_93_92 only in its own parameter declaration.  Neither had an
 * array; 16 and 40 composed writes were dropped; and both left the
 * dead-code report by disappearing rather than by coming alive, which
 * reads like a repair and is the opposite of one.
 *
 * The extent of such an array is the annotation's, which extract_array
 * already builds: see arena_floor.
 *
 * An array that had to be added here is REPORTED.  Adding it repairs the
 * analysis, but the source still never spells that name, and an annotation
 * naming storage the code reaches only under another spelling is a fact
 * the emitter wants back: at 402 nodes hca_k_all_3_350 is read through
 * "(ggml_fp16_t (*)[128]) hca_k_all_3_350" and the two spellings are two
 * arrays, so the second one belongs in the pragma as a member at offset
 * zero.  Silence here would leave that a matter of luck.
 */
static void arena_add_arrays(isl_ctx *ctx, array_desc_set &arrays)
{
	std::map<ValueDecl *, pet_arena_entry>::iterator it;
	array_desc_set::iterator a;
	std::set<std::string> known, added;

	/* BY NAME, NOT BY ID.  The ids already in the set have been through
	 * pet_expr_anonymize and carry no ValueDecl; an id built here from
	 * the declaration carries one, and isl treats the two as different
	 * objects although both print the same name.  Matching on the object
	 * therefore found nothing, reported all twelve representatives of the
	 * 402-node scop as unspelled -- including ones the source plainly
	 * subscribes, "ffn_moe_gate_3_388[t*192 + o*1] = (float) acc" -- and
	 * added a second array for each of them.  The question here is
	 * whether an array of that NAME is known, which is what is asked.
	 */
	for (a = arrays.begin(); a != arrays.end(); ++a) {
		isl_id *id;

		if (isl_id_list_n_id(*a) != 1)
			continue;
		id = isl_id_list_get_id(*a, 0);
		known.insert(isl_id_get_name(id));
		isl_id_free(id);
	}

	for (it = pet_arena_map.begin(); it != pet_arena_map.end(); ++it) {
		ValueDecl *rep = it->second.rep;
		std::string name;

		if (!rep)
			continue;
		name = rep->getNameAsString();
		if (known.count(name))
			continue;
		arrays.insert(isl_id_list_from_id(pet_id_from_decl(ctx, rep)));
		known.insert(name);
		added.insert(name);
	}

	if (added.empty())
		return;

	fprintf(stderr, "pet: arena names never spelled in the source:");
	for (std::set<std::string>::iterator a = added.begin();
	     a != added.end(); ++a)
		fprintf(stderr, " %s", a->c_str());
	fprintf(stderr, "\n");
}

/* Construct a list of pet_arrays, one for each array (or scalar)
 * accessed inside "scop", add this list to "scop" and return the result.
 * The upper bounds of the arrays are converted to affine expressions
 * within the context "pc".
 *
 * The context of "scop" is updated with the intersection of
 * the contexts of all arrays, i.e., constraints on the parameters
 * that ensure that the arrays have a valid (non-negative) size.
 *
 * If any of the extracted arrays refers to a member access or
 * has a typedef'd type as base type,
 * then also add the required types to "scop".
 * The typedef types are printed first because their definitions
 * may include the definition of a struct and these struct definitions
 * should not be printed separately.  While the typedef definition
 * is being printed, the struct is marked as having been printed as well,
 * such that the later printing of the struct by itself can be prevented.
 *
 * If the sequence of nested array declarations from which the pet_array
 * is extracted appears as the prefix of some other sequence,
 * then the pet_array is marked as "outer".
 * The arrays that already appear in scop->arrays at the start of
 * this function are assumed to be simple arrays, so they are not marked
 * as outer.
 */
struct pet_scop *PetScan::scan_arrays(struct pet_scop *scop,
	__isl_keep pet_context *pc, int arena)
{
	int i, n, n_alloc;
	array_desc_set arrays, has_sub;
	array_desc_set::iterator it;
	PetTypes types;
	std::set<TypeDecl *> types_done;
	std::set<clang::RecordDecl *, less_name>::iterator records_it;
	std::set<clang::TypedefNameDecl *, less_name>::iterator typedefs_it;
	int n_array;
	struct pet_array **scop_arrays;

	if (!scop)
		return NULL;

	pet_scop_collect_arrays(scop, arrays);
	if (arena)
		arena_add_arrays(ctx, arrays);
	if (arrays.size() == 0)
		return scop;

	n_array = scop->n_array;

	scop_arrays = isl_realloc_array(ctx, scop->arrays, struct pet_array *,
					n_array + arrays.size());
	if (!scop_arrays)
		goto error;
	scop->arrays = scop_arrays;

	for (it = arrays.begin(); it != arrays.end(); ++it) {
		isl_id_list *list = isl_id_list_copy(*it);
		int n = isl_id_list_n_id(list);
		list = isl_id_list_drop(list, n - 1, 1);
		has_sub.insert(list);
	}

	for (it = arrays.begin(), i = 0; it != arrays.end(); ++it, ++i) {
		struct pet_array *array;
		array = extract_array(*it, &types, pc);
		scop->arrays[n_array + i] = array;
		if (!scop->arrays[n_array + i])
			goto error;
		if (has_sub.find(*it) != has_sub.end())
			array->outer = 1;
		scop->n_array++;
		scop->context = isl_set_intersect(scop->context,
						isl_set_copy(array->context));
		if (!scop->context)
			goto error;
	}

	n = types.records.size() + types.typedefs.size();
	if (n == 0)
		return scop;

	n_alloc = n;
	scop->types = isl_alloc_array(ctx, struct pet_type *, n_alloc);
	if (!scop->types)
		goto error;

	for (typedefs_it = types.typedefs.begin();
	     typedefs_it != types.typedefs.end(); ++typedefs_it)
		scop = add_type(ctx, scop, *typedefs_it, PP, types,
				types_done, n_alloc);

	for (records_it = types.records.begin();
	     records_it != types.records.end(); ++records_it)
		scop = add_type(ctx, scop, *records_it, PP, types,
				types_done, n_alloc);

	return scop;
error:
	pet_scop_free(scop);
	return NULL;
}

/* Bound all parameters in scop->context to the possible values
 * of the corresponding C variable.
 */
static struct pet_scop *add_parameter_bounds(struct pet_scop *scop)
{
	int n;

	if (!scop)
		return NULL;

	n = isl_set_dim(scop->context, isl_dim_param);
	for (int i = 0; i < n; ++i) {
		isl_id *id;
		ValueDecl *decl;

		id = isl_set_get_dim_id(scop->context, isl_dim_param, i);
		if (pet_nested_in_id(id)) {
			isl_id_free(id);
			isl_die(isl_set_get_ctx(scop->context),
				isl_error_internal,
				"unresolved nested parameter", goto error);
		}
		decl = pet_id_get_decl(id);
		isl_id_free(id);

		scop->context = set_parameter_bounds(scop->context, i, decl);

		if (!scop->context)
			goto error;
	}

	return scop;
error:
	pet_scop_free(scop);
	return NULL;
}

/* Construct a pet_scop from the given function.
 *
 * If the scop was delimited by scop and endscop pragmas, then we override
 * the file offsets by those derived from the pragmas.
 *
 * A return at the end of the body is a statement like any other here.
 * What a return does that a scop cannot hold is leave in the middle --
 * and the last statement of a body leaves nothing behind it, so there is
 * nothing to hold.  It is the same reason the body put in place of a call
 * is scanned with the same permission (see extract_inlined_call), and
 * where the return is not last, final_in_compound still refuses.
 *
 * Without this every function written as one line -- the whole of a C
 * interface, "return what_it_really_calls(...)" -- gives a scop of
 * nothing, and the body put in place of the call goes with it.  Over the
 * 51 units of an engine that was 2304 of the 4840 places a scop ended,
 * more than every other reason together.
 */
struct pet_scop *PetScan::scan(FunctionDecl *fd)
{
	pet_scop *scop;
	Stmt *stmt;

	stmt = fd->getBody();
	return_root = stmt;

	if (options->autodetect) {
		set_current_stmt(stmt);
		scop = extract_scop(extract(stmt, true));
	} else {
		current_line = loc.start_line;
		scop = scan(stmt);
		scop = pet_scop_update_start_end(scop, loc.start, loc.end);
	}
	scop = add_parameter_bounds(scop);
	scop = pet_scop_gist(scop, value_bounds);

	return scop;
}

namespace {

/* Helper class for detecting recursive types.
 */
struct RecursionDetector {
	/* The current outer types. */
	std::set<TypeDecl *> outer;

	bool is_recursive(RecordDecl *decl);
	bool is_recursive(QualType qt);
};

}

/* Is the type containing the record declaration "decl" recursive?
 *
 * If the type appears within its own declaration or
 * if this holds for any part of any of its fields,
 * then the type is recursive.
 */
bool RecursionDetector::is_recursive(RecordDecl *decl)
{
	RecordDecl::field_iterator it;

	if (outer.find(decl) != outer.end())
		return true;

	outer.insert(decl);

	for (it = decl->field_begin(); it != decl->field_end(); ++it)
		if (is_recursive(it->getType()))
			return true;

	outer.erase(decl);

	return false;
}

/* Is the type containing "qt" recursive?
 *
 * If "qt" is a recursive record, then this is the case.
 */
bool RecursionDetector::is_recursive(QualType qt)
{
	QualType base = pet_clang_base_type(qt);

	if (base->isRecordType()) {
		RecordDecl *rec = pet_clang_record_decl(base);

		return rec ? is_recursive(rec) : false;
	}
	return false;
}

/* Count the number of AST Stmt nodes in the body of "fd", cached.
 *
 * The count is shared across the whole walk and computed at most once
 * per function: many calls to the same function hit the cache.
 */
int PetScan::count_body_stmts(FunctionDecl *fd)
{
	auto it = walk->body_stmt_count.find(fd);
	if (it != walk->body_stmt_count.end())
		return it->second;

	int count = 0;
	Stmt *body = fd->getBody();
	if (body) {
		struct StmtCounter : RecursiveASTVisitor<StmtCounter> {
			int &n;
			StmtCounter(int &n) : n(n) {}
			bool VisitStmt(Stmt *s) { ++n; return true; }
		};
		StmtCounter counter(count);
		counter.TraverseStmt(body);
	}
	walk->body_stmt_count[fd] = count;
	return count;
}

/* Is the type "qt" recursive?
 *
 * If this property has been determined before, then reuse the result.
 */
bool PetScan::is_recursive(QualType qt)
{
	const Type *type = qt.getTypePtr();

	if (recursive.find(type) == recursive.end())
		recursive[type] = RecursionDetector().is_recursive(qt);
	return recursive[type];
}

/* Update this->last_line and this->current_line based on the fact
 * that we are about to consider "stmt".
 */
void PetScan::set_current_stmt(Stmt *stmt)
{
	SourceLocation loc = begin_loc(stmt);
	SourceManager &SM = PP.getSourceManager();

	last_line = current_line;
	current_line = SM.getExpansionLineNumber(loc);
}

/* Is the current statement marked by an independent pragma?
 * That is, is there an independent pragma on a line between
 * the line of the current statement and the line of the previous statement.
 * The search is not implemented very efficiently.  We currently
 * assume that there are only a few independent pragmas, if any.
 */
bool PetScan::is_current_stmt_marked_independent()
{
	for (unsigned i = 0; i < independent.size(); ++i) {
		unsigned line = independent[i].line;

		if (last_line < line && line < current_line)
			return true;
	}

	return false;
}
