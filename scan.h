#ifndef PET_SCAN_H
#define PET_SCAN_H

#include <set>
#include <map>

#include <clang/Basic/SourceManager.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Stmt.h>
#include <clang/Lex/Preprocessor.h>

#include <isl/ctx.h>
#include <isl/map.h>
#include <isl/val.h>

#include "context.h"
#include "inliner.h"
#include "isl_id_to_pet_expr.h"
#include "loc.h"
#include "scop.h"
#include "summary.h"
#include "tree.h"

#include "config.h"

namespace clang {

#ifndef HAVE_STMTRANGE
/* StmtRange was replaced by iterator_range in more recent versions of clang.
 * Implement a StmtRange in terms of this iterator_range if StmtRange
 * is not available.
 */
struct StmtRange : std::pair<StmtIterator,StmtIterator> {
	StmtRange(const StmtIterator &begin, const StmtIterator &end) :
		std::pair<StmtIterator,StmtIterator>(begin, end) {}
	StmtRange(Stmt::child_range range) :
		std::pair<StmtIterator,StmtIterator>(range.begin(),
							range.end()) {}
};
#endif

}

/* The location of the scop, as delimited by scop and endscop
 * pragmas by the user.
 * "scop" and "endscop" are the source locations of the scop and
 * endscop pragmas.
 * "start_line" is the line number of the start position.
 * "name" is the identifier the scop pragma was written with, or empty
 * if it was written without one.  A scop is otherwise known only by
 * where it stands, which is no use to anything that has to name it
 * from the outside.
 */
struct ScopLoc {
	ScopLoc() : end(0) {}

	clang::SourceLocation scop;
	clang::SourceLocation endscop;
	unsigned start_line;
	unsigned start;
	unsigned end;
	std::string name;
};

/* The information extracted from a pragma pencil independent.
 * We currently only keep track of the line number where
 * the pragma appears.
 */
struct Independent {
	Independent(unsigned line) : line(line) {}

	unsigned line;
};

/* Compare two TypeDecl pointers based on their names.
 */
struct less_name {
	/* The name to order by.
	 *
	 * Only a name that is a plain identifier is used.  Writing out
	 * any other kind means printing a declaration name, which for
	 * some of the names a C++ program gives its types reaches for
	 * things that are not there.  Those all order together, which
	 * costs nothing: a type pet cannot name is one it cannot write
	 * out either.
	 */
	static std::string key(const clang::TypeDecl *decl) {
		clang::DeclarationName name = decl->getDeclName();

		if (name.isIdentifier() && name.getAsIdentifierInfo())
			return name.getAsIdentifierInfo()->getName().str();

		return std::string();
	}
	bool operator()(const clang::TypeDecl *x,
			const clang::TypeDecl *y) const {
		return key(x).compare(key(y)) < 0;
	}
};

/* The PetTypes structure collects a set of RecordDecl and
 * TypedefNameDecl pointers.
 * The pointers are sorted using a fixed order.  The actual order
 * is not important, only that it is consistent across platforms.
 */
struct PetTypes {
	std::set<clang::RecordDecl *, less_name> records;
	std::set<clang::TypedefNameDecl *, less_name> typedefs;

	void insert(clang::RecordDecl *decl) {
		records.insert(decl);
	}
	void insert(clang::TypedefNameDecl *decl) {
		typedefs.insert(decl);
	}
};

/* How far a body is put in place of a call to it.
 *
 * A cycle is caught for what it is; this is the second line, for a
 * chain of calls that is merely long, and it is set where the chains of
 * a real program do not reach.
 */
static const int max_inline_depth = 32;

/* How many AST statement nodes are put in place of calls at most,
 * going over one function.
 *
 * A cycle is caught by the set and a long chain by the depth, and
 * neither catches what is neither: a tree of calls that does not repeat
 * and is not deep, but is wide at every level.  GGML_ASSERT alone
 * reaches ggml_abort, which reaches the printing of a backtrace, which
 * reaches the standard library, and thirty-two levels of that is a
 * number of paths no machine finishes.
 *
 * Instead of counting the number of calls (which says nothing about how
 * large each inlined body is), this counts the actual AST Stmt nodes
 * being inlined.  A function of two thousand statements counts for two
 * thousand, not for one, and many small helpers total up honestly.
 */
static const int max_inlined_stmts = 100000;

/* What a walk over one function shares with every walk it starts.
 *
 * Going over a body starts a walk of its own -- for the summary of what
 * it calls, and for the body put in place of a call -- and each of those
 * starts more.  What holds them back has to be one thing held in common:
 * a guard kept by one of the two ways down is not there when the other
 * way is taken, and the counting begins again.  That is not a thought
 * about how it might go wrong; it is what it did, going round eighteen
 * thousand times with a depth of thirty-two in force, three bodies put
 * in place for every summary asked after.
 */
struct pet_walk {
	/* The functions whose summary is being worked out. */
	std::set<clang::FunctionDecl *> in_summary;
	/* The functions whose body is being put in place of a call. */
	std::set<clang::FunctionDecl *> inlining;
	/* How far down each of the two goes right now. */
	int summary_depth;
	int inline_depth;
	/* How many AST statement nodes have been put in place at all,
	 * over every function body inlined so far. */
	int total_stmts;
	/* How many AST statement nodes each function body holds.
	 * Counted once and cached; many calls to the same function
	 * check the cache and do not recount. */
	std::map<clang::FunctionDecl *, int> body_stmt_count;

	/* How many variables holding the return value of an inlined call
	 * have been named so far.  It counts over the whole walk and not
	 * over one scan: a body put in place of a call is read by a scan
	 * of its own, and a counter belonging to each scan starts again
	 * at zero for every body, so two bodies inlined into one scop
	 * both name their variable __pet_ret_0 and the scop declares
	 * that name twice.
	 */
	int n_ret;

	/* How many temporary variables holding an index of an array
	 * argument of an inlined call have been named so far.  It counts
	 * over the whole walk and not over one scan, for the same reason
	 * as n_ret: a body put in place of a call is read by a scan of
	 * its own, so a counter belonging to each scan starts again at
	 * zero for every body.  A call inside an inlined body then names
	 * its index __pet_arg_0 again, and since an isl_id is its name,
	 * that is not a second variable but the very same one: the outer
	 * index is overwritten by the inner one before it is read, and
	 * the scop says the call wrote somewhere it did not.
	 */
	int n_arg;

	/* Per-guard firing counters for already_inlining diagnostics. */
	int n_no_return;
	int n_depth;
	int n_too_many_stmts;
	int n_cycle;

	/* What each DeclContext calls the things it holds, worked out
	 * once per context and kept for the whole walk.
	 *
	 * Asking whether a name is taken means asking every enclosing
	 * context, and the outermost of them is the translation unit.
	 * Read from one file that is a few thousand declarations; read
	 * from a link of a program it is every declaration of every unit,
	 * and the question is asked once for every argument of every
	 * body put in place of a call.  It belongs to the walk rather
	 * than to a scan because a body is read by a scan of its own,
	 * and a cache belonging to each scan is built again from nothing
	 * for every body inlined.
	 *
	 * "first" is the one declaration of that name where there is
	 * only one, so that a name can still be found free for the
	 * declaration that already carries it; "many" holds the names
	 * that more than one declaration carries, which no single
	 * declaration can be excused from.
	 */
	struct context_names {
		std::map<std::string, clang::Decl *> first;
		std::set<std::string> many;
	};
	std::map<clang::DeclContext *, context_names> names_of;

	pet_walk() : summary_depth(0), inline_depth(0), total_stmts(0),
		n_ret(0), n_arg(0), n_no_return(0), n_depth(0),
		n_too_many_stmts(0), n_cycle(0) {}
};

struct PetScan {
	clang::Preprocessor &PP;
	clang::ASTContext &ast_context;
	/* The DeclContext of the function containing the scop.
	 */
	clang::DeclContext *decl_context;
	/* If autodetect is false, then loc contains the location
	 * of the scop to be extracted.
	 */
	ScopLoc &loc;
	isl_ctx *ctx;
	pet_options *options;
	/* If not NULL, then return_root represents the compound statement
	 * in which a return statement is allowed as the final child.
	 * If return_root is NULL, then no return statements are allowed.
	 */
	clang::Stmt *return_root;
	/* Set if the pet_scop returned by an extract method only
	 * represents part of the input tree.
	 */
	bool partial;

	/* A cache of size expressions for array identifiers as computed
	 * by PetScan::get_array_size, or set by PetScan::set_array_size.
	 */
	isl_id_to_pet_expr *id_size;
	/* A cache of size expressions for array types as computed
	 * by PetScan::get_array_size.
	 */
	std::map<const clang::Type *, pet_expr *> type_size;

	/* A dummy summary indicating that no summary could be constructed.
	 */
	pet_function_summary *no_summary;
	/* A cache of function summaries for function declarations
	 * as extracted by PetScan::get_summary.
	 */
	std::map<clang::FunctionDecl *, pet_function_summary *> summary_cache;

	/* A union of mappings of the form
	 *	{ identifier[] -> [i] : lower_bound <= i <= upper_bound }
	 */
	isl_union_map *value_bounds;

	/* The line number of the previously considered Stmt. */
	unsigned last_line;
	/* The line number of the Stmt currently being considered. */
	unsigned current_line;
	/* Information about the independent pragmas in the source code. */
	std::vector<Independent> &independent;

	/* All variables that have already been declared
	 * in the current compound statement.
	 */
	std::vector<clang::VarDecl *> declarations;
	/* Sequence number of the next rename. */
	int n_rename;
	/* Have the declared names been collected? */
	bool declared_names_collected;
	/* The names of the variables declared in decl_context,
	 * if declared_names_collected is set.
	 */
	std::set<std::string> declared_names;
	/* A set of names known to be in use. */
	std::set<std::string> used_names;

	/* If not NULL, then "call2id" maps inlined call expressions
	 * that return a value to the corresponding variables.
	 */
	std::map<clang::Stmt *, isl_id *> *call2id;

	/* The functions whose summary is being worked out right now, and
	 * how many of them there are.
	 *
	 * Working out the summary of a function means going over its body,
	 * and going over a body means asking for the summary of what it
	 * calls.  Two functions that call each other therefore ask after
	 * each other for as long as there is stack to ask with.  The cache
	 * does not stop it: an answer is put there once it has been worked
	 * out, and a cycle is exactly the case where it never is.  Nor is
	 * one cache enough, since each of these goes over the body with a
	 * scan of its own; the set is shared down the whole nest instead.
	 */
	/* What this walk shares with the walks it starts.  Every scan has
	 * one of its own to begin with, and every scan it starts is given
	 * the same one.
	 */
	pet_walk own_walk;
	pet_walk *walk;

	/* Where the first place a scop stopped was, and what stopped it.
	 *
	 * A scan meets many such places -- ten thousand over a function is
	 * not unusual -- and they are all written out when asked for.  But
	 * a map of a program wants one line for each function, and what
	 * belongs on that line is the first: the others are what was left
	 * over after the scop had already ended.
	 */
	std::string first_stop;

	/* And the last of them.
	 *
	 * Where a scop came out, the first place it stopped is its
	 * border and the rest is what lies beyond.  Where none came out
	 * there is no border, and the first place says only what was met
	 * first -- which, once a body is put in place of a call, is
	 * something about that body rather than about the function being
	 * asked after.  The last is the one the scan gave up nearest to.
	 */
	std::string last_stop;

	/* The functions whose bodies are being put in place of a call to
	 * them right now, and how many of them there are.
	 *
	 * Putting a body in place of a call means going over that body,
	 * and going over a body means meeting the calls it makes.  Two
	 * functions that call each other would be put inside each other
	 * for as long as there is stack, and one that calls itself would
	 * not even need a second.  As with a summary, each body is gone
	 * over by a scan of its own, so the set is shared down the nest
	 * rather than kept by any one of them.
	 */
	/* Is the body of "fd" one to leave where it is?
	 *
	 * A body already being put in place would be put inside itself;
	 * one deeper than bodies are followed, or one past the point
	 * where enough of them have been put in place, is where this
	 * stops of its own accord.  Why is said, once, so that a scop
	 * that did not grow says what stopped it growing.
	 */
	bool already_inlining(clang::FunctionDecl *fd) {
		const char *why = NULL;

		if (fd->isNoReturn()) {
			walk->n_no_return++;
			why = "it does not return";
		}
		else if (walk->inline_depth >= max_inline_depth) {
			walk->n_depth++;
			why = "bodies are not followed that deep";
		}
		else if (walk->total_stmts + count_body_stmts(fd) > max_inlined_stmts) {
			walk->n_too_many_stmts++;
			why = "enough statement nodes have been put in place already";
		}
		else if (walk->inlining.find(fd) != walk->inlining.end()) {
			walk->n_cycle++;
			why = "it is already being put in place";
		}
		else
			return false;

		if (first_stop.empty())
			first_stop = "the body of " + fd->getNameAsString() +
					" was left where it is: " + why;

		return true;
	}

	PetScan(clang::Preprocessor &PP, clang::ASTContext &ast_context,
		clang::DeclContext *decl_context, ScopLoc &loc,
		pet_options *options, __isl_take isl_union_map *value_bounds,
		std::vector<Independent> &independent) :
		PP(PP),
		ast_context(ast_context), decl_context(decl_context), loc(loc),
		ctx(isl_union_map_get_ctx(value_bounds)),
		options(options), return_root(NULL), partial(false),
		no_summary(pet_function_summary_alloc(ctx, 0)),
		value_bounds(value_bounds), last_line(0), current_line(0),
		independent(independent), n_rename(0),
		declared_names_collected(false), call2id(NULL),
		walk(&own_walk) {
		id_size = isl_id_to_pet_expr_alloc(ctx, 0);
	}

	~PetScan();

	struct pet_scop *scan(clang::FunctionDecl *fd);

	static __isl_give isl_val *extract_int(isl_ctx *ctx,
		clang::IntegerLiteral *expr);
	__isl_give pet_expr *get_array_size(__isl_keep isl_id *id);
	void set_array_size(__isl_take isl_id *id, __isl_take pet_expr *size);
	struct pet_array *extract_array(__isl_keep isl_id *id,
		PetTypes *types, __isl_keep pet_context *pc);
	__isl_give pet_tree *extract_inlined_call(clang::CallExpr *call,
		clang::FunctionDecl *fd, __isl_keep isl_id *return_id);
private:
	/* For each type that has been examined already, is it recursive?
	 */
	std::map<const clang::Type *, bool> recursive;
	bool is_recursive(clang::QualType qt);
		int count_body_stmts(clang::FunctionDecl *fd);

	void set_current_stmt(clang::Stmt *stmt);
	bool is_current_stmt_marked_independent();

	void collect_declared_names();
	void add_new_used_names(const std::set<std::string> &used_names);
	bool name_in_use(const std::string &name, clang::Decl *decl);
	std::string generate_new_name(const std::string &name);

	__isl_give pet_tree *add_kills(__isl_take pet_tree *tree,
		std::set<clang::ValueDecl *> locals);

	struct pet_scop *scan(clang::Stmt *stmt);

	struct pet_scop *scan_arrays(struct pet_scop *scop,
		__isl_keep pet_context *pc);
	struct pet_array *extract_array(clang::ValueDecl *decl,
		PetTypes *types, __isl_keep pet_context *pc);
	struct pet_array *extract_array(__isl_keep isl_id_list *decls,
		PetTypes *types, __isl_keep pet_context *pc);
	__isl_give pet_expr *set_upper_bounds(__isl_take pet_expr *expr,
		clang::QualType qt, int pos);
	struct pet_array *set_upper_bounds(struct pet_array *array,
		__isl_keep pet_context *pc);
	int substitute_array_sizes(__isl_keep pet_tree *tree,
		pet_substituter *substituter);

	__isl_give pet_tree *insert_initial_declarations(
		__isl_take pet_tree *tree, int n_decl,
		clang::StmtRange stmt_range);
	__isl_give pet_tree *extract(clang::Stmt *stmt,
		bool skip_declarations = false);
	__isl_give pet_tree *extract(clang::StmtRange stmt_range, bool block,
		bool skip_declarations, clang::Stmt *parent);
	__isl_give pet_tree *extract(clang::IfStmt *stmt);
	__isl_give pet_tree *extract(clang::WhileStmt *stmt);
	__isl_give pet_tree *extract(clang::CompoundStmt *stmt,
		bool skip_declarations = false);
	__isl_give pet_tree *extract(clang::LabelStmt *stmt);
	__isl_give pet_tree *extract(clang::Decl *decl);
	__isl_give pet_tree *extract(clang::DeclStmt *expr);
	__isl_give pet_tree *extract(clang::ReturnStmt *stmt);

	__isl_give pet_loc *construct_pet_loc(clang::SourceRange range,
		bool skip_semi);
	__isl_give pet_tree *extract(__isl_take pet_expr *expr,
		clang::SourceRange range, bool skip_semi);
	__isl_give pet_tree *update_loc(__isl_take pet_tree *tree,
		clang::Stmt *stmt);

	struct pet_scop *extract_scop(__isl_take pet_tree *tree);

	clang::BinaryOperator *initialization_assignment(clang::Stmt *init);
	clang::Decl *initialization_declaration(clang::Stmt *init);
	clang::ValueDecl *extract_induction_variable(clang::BinaryOperator *stmt);
	clang::VarDecl *extract_induction_variable(clang::Stmt *init,
				clang::Decl *stmt);
	__isl_give pet_expr *extract_unary_increment(clang::UnaryOperator *op,
				clang::ValueDecl *iv);
	__isl_give pet_expr *extract_binary_increment(
				clang::BinaryOperator *op,
				clang::ValueDecl *iv);
	__isl_give pet_expr *extract_compound_increment(
				clang::CompoundAssignOperator *op,
				clang::ValueDecl *iv);
	__isl_give pet_expr *extract_increment(clang::ForStmt *stmt,
				clang::ValueDecl *iv);
	__isl_give pet_tree *extract_for(clang::ForStmt *stmt);
	__isl_give pet_tree *extract_expr_stmt(clang::Stmt *stmt);
	int set_inliner_arguments(pet_inliner &inliner, clang::CallExpr *call,
		clang::FunctionDecl *fd);

	__isl_give pet_expr *extract_assume(clang::Expr *expr);
	__isl_give pet_function_summary *cache_summary(clang::FunctionDecl *fd,
		__isl_take pet_function_summary *summary);
	__isl_give pet_function_summary *get_summary_from_tree(
		__isl_take pet_tree *tree, clang::FunctionDecl *fd,
		PetScan &body_scan);
	__isl_give pet_function_summary *get_summary(clang::FunctionDecl *fd);
	__isl_give pet_expr *set_summary(__isl_take pet_expr *expr,
		clang::FunctionDecl *fd);
	__isl_give pet_expr *extract_argument(clang::FunctionDecl *fd, int pos,
		clang::Expr *expr, bool detect_writes);
	__isl_give pet_expr *extract_expr(const llvm::APInt &val);
	__isl_give pet_expr *extract_expr(clang::Expr *expr);
	__isl_give pet_expr *extract_expr(clang::UnaryOperator *expr);
	__isl_give pet_expr *extract_expr(clang::BinaryOperator *expr);
	__isl_give pet_expr *extract_expr(clang::ImplicitCastExpr *expr);
	__isl_give pet_expr *extract_expr(clang::IntegerLiteral *expr);
	__isl_give pet_expr *extract_expr(clang::EnumConstantDecl *expr);
	__isl_give pet_expr *extract_expr(clang::FloatingLiteral *expr);
	__isl_give pet_expr *extract_expr(clang::ParenExpr *expr);
	__isl_give pet_expr *extract_expr(clang::ConditionalOperator *expr);
	__isl_give pet_expr *extract_expr(clang::CallExpr *expr);
	__isl_give pet_expr *extract_expr(clang::CStyleCastExpr *expr);

	__isl_give pet_expr *extract_access_expr(clang::Expr *expr);
	__isl_give pet_expr *union_member_storage(clang::MemberExpr *member);
	__isl_give pet_expr *access_from_union_member(clang::Expr *expr,
		__isl_take pet_expr *index);
	__isl_give pet_expr *extract_access_expr(clang::ValueDecl *decl);

	__isl_give pet_expr *extract_index_expr(
		clang::ArraySubscriptExpr *expr);
	__isl_give pet_expr *extract_index_expr(clang::Expr *expr);
	__isl_give pet_expr *extract_index_expr(clang::ImplicitCastExpr *expr);
	__isl_give pet_expr *extract_index_expr(clang::DeclRefExpr *expr);
	__isl_give pet_expr *extract_index_expr(clang::ValueDecl *decl);
	__isl_give pet_expr *extract_index_expr(clang::MemberExpr *expr);

	__isl_give isl_val *extract_int(clang::Expr *expr);
	__isl_give isl_val *extract_int(clang::ParenExpr *expr);

	clang::FunctionDecl *find_decl_from_name(clang::CallExpr *call,
		std::string name);
public:
	clang::FunctionDecl *get_summary_function(clang::CallExpr *call);
private:

	void stopped_at(clang::SourceRange range, const std::string &why);
	void report(clang::SourceRange range, unsigned id);
	void report(clang::Stmt *stmt, unsigned id);
	void report(clang::Decl *decl, unsigned id);
	void unsupported(clang::Stmt *stmt);
	void report_unsupported_unary_operator(clang::Stmt *stmt);
	void report_unsupported_union_member_size(clang::Stmt *stmt);
	void report_unsupported_binary_operator(clang::Stmt *stmt);
	void report_unsupported_statement_type(clang::Stmt *stmt);
	void report_prototype_required(clang::Stmt *stmt);
	void report_missing_increment(clang::Stmt *stmt);
	void report_missing_summary_function(clang::Stmt *stmt);
	void report_missing_summary_function_body(clang::Stmt *stmt);
	void report_unsupported_inline_function_argument(clang::Stmt *stmt);
	void report_unsupported_declaration(clang::Decl *decl);
	void report_unbalanced_pragmas(clang::SourceLocation scop,
		clang::SourceLocation endscop);
	void report_unsupported_return(clang::Stmt *stmt);
	void report_return_not_at_end_of_function(clang::Stmt *stmt);
	void report_unsupported_recursive_type(clang::Decl *decl);
};

#endif
