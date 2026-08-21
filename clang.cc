/*
 * Copyright 2011      Leiden University. All rights reserved.
 * Copyright 2013 Ecole Normale Superieure. All rights reserved.
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
#include "clang.h"

using namespace clang;

/* Return the element type of the given array type.
 */
QualType pet_clang_base_type(QualType qt)
{
	const Type *type = qt.getTypePtr();

	if (type->isPointerType())
		return pet_clang_base_type(type->getPointeeType());
	if (type->isArrayType()) {
		const ArrayType *atype;
		type = type->getCanonicalTypeInternal().getTypePtr();
		atype = cast<ArrayType>(type);
		return pet_clang_base_type(atype->getElementType());
	}
	return qt;
}

/* Return the first typedef type that "qt" points to
 * or the base type if there is no such typedef type.
 * Do not call getCanonicalTypeInternal as in pet_clang_base_type
 * because that throws away all internal typedef types.
 * Look through any ElaboratedType sugar, on the versions of clang
 * that still have it.
 */
QualType pet_clang_base_or_typedef_type(QualType qt)
{
	const Type *type = qt.getTypePtr();

#ifdef HAVE_ELABORATEDTYPE
	if (isa<ElaboratedType>(type)) {
		qt = cast<ElaboratedType>(type)->desugar();
		return pet_clang_base_or_typedef_type(qt);
	}
#endif
	if (isa<TypedefType>(type))
		return qt;
	if (type->isPointerType())
		return pet_clang_base_type(type->getPointeeType());
	if (type->isArrayType()) {
		const ArrayType *atype;
		atype = cast<ArrayType>(type);
		return pet_clang_base_type(atype->getElementType());
	}
	return qt;
}

/* The RecordDecl of "T", or NULL when "T" is not a record type.
 *
 * A C++ type that names a class need not be a record type once it is
 * asked what it is: the name of a class inside its own definition, and
 * one that is still written in terms of a template parameter, are types
 * with no declaration to return.
 */
RecordDecl *pet_clang_record_decl(QualType T)
{
	const Type *type = T->getCanonicalTypeInternal().getTypePtr();
	const RecordType *record;

	record = dyn_cast<RecordType>(type);
	if (!record)
		return NULL;

	return record->getDecl();
}

/* Strip off all outer casts from "expr" that are either implicit or a no-op.
 */
Expr *pet_clang_strip_casts(Expr *expr)
{
	while (isa<CastExpr>(expr)) {
		CastExpr *ce = cast<CastExpr>(expr);
		CastKind kind = ce->getCastKind();
		if (!isa<ImplicitCastExpr>(expr) && kind != CK_NoOp)
			break;
		expr = ce->getSubExpr();
	}

	return expr;
}

/* Return the number of bits needed to represent the type "qt",
 * if it is an integer type.  Otherwise return 0.
 * If qt is signed then return the opposite of the number of bits.
 */
int pet_clang_get_type_size(QualType qt, ASTContext &ast_context)
{
	int size;

	if (!qt->isIntegerType())
		return 0;

	size = ast_context.getIntWidth(qt);
	if (!qt->isUnsignedIntegerType())
		size = -size;

	return size;
}

/* Return the FunctionDecl that refers to the same function
 * that "fd" refers to, but that has a body.
 * Return NULL if no such FunctionDecl is available.
 *
 * It is not clear why hasBody takes a reference to a const FunctionDecl *.
 * It seems that it is possible to directly use the iterators to obtain
 * a non-const pointer.
 * Since we are not going to use the pointer to modify anything anyway,
 * it seems safe to drop the constness.  The alternative would be to
 * modify a lot of other functions to include const qualifiers.
 */
FunctionDecl *pet_clang_find_function_decl_with_body(FunctionDecl *fd)
{
	const FunctionDecl *def;

	/* A call through a function pointer names no declaration at all,
	 * and one that names none has no body to find.
	 */
	if (!fd)
		return NULL;
	if (!fd->hasBody(def))
		return NULL;

	return const_cast<FunctionDecl *>(def);
}

/* Which function does "call" call?
 *
 * Ordinarily the call names it, and clang says which.  Where it does
 * not, the name written may still be a name for exactly one function:
 *
 *	constexpr auto to_f32 = type_conversion_table<src_t>::to_f32;
 *	...
 *	y[i] = to_f32(x[i]);
 *
 * is how ggml writes the conversion of one element, and to_f32 is a
 * constant whose value is the address of a function.  Nothing about
 * such a call is decided while the program runs; only the way it is
 * written keeps clang from calling it direct.
 *
 * The constant is followed as far as it goes: the one written in the
 * body names another, the static member of the table, and that one
 * names the function.  Each step is a constant whose initialiser is one
 * name, so each step has one answer; a few steps are allowed and no
 * more, since a constant that names itself would otherwise be followed
 * for as long as there is patience.
 *
 * Only a constant, only an initialiser that names one thing, and only a
 * name, never an expression: a call this cannot follow stays a call
 * that names nothing, and says so.
 */
FunctionDecl *pet_clang_direct_callee(CallExpr *call)
{
	Expr *callee;
	ValueDecl *named;
	FunctionDecl *fd = call->getDirectCallee();

	if (fd)
		return fd;

	callee = call->getCallee();
	if (!callee)
		return NULL;

	DeclRefExpr *ref = dyn_cast<DeclRefExpr>(callee->IgnoreParenImpCasts());

	if (!ref)
		return NULL;
	named = ref->getDecl();

	for (int step = 0; step < 4; ++step) {
		VarDecl *var = dyn_cast<VarDecl>(named);
		const Expr *init;

		if (isa<FunctionDecl>(named))
			return cast<FunctionDecl>(named);
		if (!var || !var->isConstexpr())
			return NULL;
		init = var->getAnyInitializer();
		if (!init)
			return NULL;

		init = init->IgnoreParenImpCasts();
		if (isa<UnaryOperator>(init) &&
		    cast<UnaryOperator>(init)->getOpcode() == UO_AddrOf)
			init = cast<UnaryOperator>(init)->getSubExpr()->
							IgnoreParenImpCasts();

		ref = dyn_cast<DeclRefExpr>(const_cast<Expr *>(init));
		if (!ref)
			return NULL;
		named = ref->getDecl();
	}

	return NULL;
}

/* Return the depth of an array of the given type.
 */
int pet_clang_array_depth(QualType qt)
{
	const Type *type = qt.getTypePtr();

	if (type->isPointerType())
		return 1 + pet_clang_array_depth(type->getPointeeType());
	if (type->isArrayType()) {
		const ArrayType *atype;
		type = type->getCanonicalTypeInternal().getTypePtr();
		atype = cast<ArrayType>(type);
		return 1 + pet_clang_array_depth(atype->getElementType());
	}
	return 0;
}
