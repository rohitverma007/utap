// -*- mode: C++; c-file-style: "stroustrup"; c-basic-offset: 4; -*-

/* libutap - Uppaal Timed Automata Parser.
   Copyright (C) 2002,2003 Uppsala University and Aalborg University.
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA
*/

#include <cmath>
#include <cstdio>
#include <cassert>

#include "utap/utap.hh"
#include "utap/typechecker.hh"
#include "utap/systembuilder.hh"

using std::exception;
using std::set;
using std::pair;
using std::make_pair;
using std::max;
using std::min;
using std::map;
using std::vector;

using namespace UTAP;
using namespace Constants;

class InitialiserException : public std::exception
{
private:
    expression_t expr;
    char msg[256];
public:
    InitialiserException(expression_t expr, const char *m):
	expr(expr) { strncpy(msg, m, 256); }
    ~InitialiserException() throw() {}
    expression_t getExpression() { return expr; }
    const char *what() const throw() { return msg; }
};

void PersistentVariables::visitVariable(variable_t &variable)
{
    if (!variable.uid.getType().hasPrefix(prefix::CONSTANT))
	variables.insert(variable.uid);
}

void PersistentVariables::visitTemplateAfter(template_t &temp)
{
    frame_t parameters = temp.uid.getType().getFrame();
    for (uint32_t i = 0; i < parameters.getSize(); i++) 
	if (parameters[i].getType().hasPrefix(prefix::REFERENCE)
	    || !parameters[i].getType().hasPrefix(prefix::CONSTANT))
	{
	    variables.insert(parameters[i]);
	}
}

const set<symbol_t> &PersistentVariables::get() const
{
    return variables;
}

TypeChecker::TypeChecker(ErrorHandler *handler) : ContextVisitor(handler)
{

}

void TypeChecker::visitSystemBefore(TimedAutomataSystem *value)
{
    system = value;
    system->accept(persistentVariables);
}

/** Annotate the expression and check that it is a constant integer. */
void TypeChecker::annotateAndExpectConstantInteger(expression_t expr)
{
    annotate(expr);

    if (!isInteger(expr))
    {
	handleError(expr, "Integer expression expected");
    }
    else if (expr.dependsOn(persistentVariables.get()))
    {
	handleError(expr, "Constant expression expected");
    }
}

/** Check that the type is type correct (i.e. all expression such
 *  as array sizes, integer ranges, etc. contained in the type).
 */
void TypeChecker::checkType(type_t type)
{
    type_t base = type.getBase();
    if (base == type_t::INT)
    {
	expression_t lower = type.getRange().first;
	expression_t upper = type.getRange().second;

	// Check if there is a range; if not then return
	if (lower.empty())
	    return;

	annotateAndExpectConstantInteger(lower);
	annotateAndExpectConstantInteger(upper);
    }
    else if (base == type_t::ARRAY)
    {
	annotateAndExpectConstantInteger(type.getArraySize());
	checkType(type.getSub());
    }
    else if (base == type_t::RECORD)
    {
	// FIXME: Not implemented
	assert(0);
    }
}

void TypeChecker::checkVariableDeclaration(variable_t &variable)
{
    setContextDeclaration();
    checkType(variable.uid.getType());
    checkInitialiser(variable);
}

void TypeChecker::visitConstant(variable_t &constant)
{
    checkVariableDeclaration(constant);
}

void TypeChecker::visitVariable(variable_t &variable)
{
    checkVariableDeclaration(variable);
}

void TypeChecker::visitState(state_t &state)
{
    if (!state.invariant.empty()) {
	setContextInvariant(state);
	annotate(state.invariant);
    
	if (!isInvariant(state.invariant))
	    handleError(state.invariant, "Invalid invariant expression");
	if (!isSideEffectFree(state.invariant))
	    handleError(state.invariant, "Invariant must be side effect free");
    }
}

void TypeChecker::visitTransition(transition_t &transition)
{
    // guard
    setContextGuard(transition);
    annotate(transition.guard);
    
    if (!isGuard(transition.guard))
	handleError(transition.guard, "Invalid guard");
    else if (!isSideEffectFree(transition.guard))
	handleError(transition.guard, "Guard must be side effect free");

    // sync
    if (!transition.sync.empty()) {
	setContextSync(transition);
	annotate(transition.sync);    

	if (!isSideEffectFree(transition.sync))
	    handleError(transition.sync,
			"Synchronisation must be side effect free");

	type_t channel = transition.sync.get(0).getType();
	assert(channel.getBase() == type_t::CHANNEL);

	bool hasClockGuard =
	    !transition.guard.empty() && !isInteger(transition.guard);
	bool isUrgent = channel.hasPrefix(prefix::URGENT);
	bool receivesBroadcast = channel.hasPrefix(prefix::BROADCAST) 
	    && transition.sync.getSync() == SYNC_QUE;
	
	if (isUrgent && hasClockGuard) {
	    handleError(transition.sync,
			"Clock guards are not allowed on urgent transitions.");
	}
	
	if (receivesBroadcast && hasClockGuard) {
	    handleError(transition.sync,
			"Clock guards are not allowed on broadcast receivers.");
	}
    }
    
    // assignment
    setContextAssignment(transition);
    annotate(transition.assign);    
    if (!isInteger(transition.assign)
	&& !isClock(transition.assign)
	&& !isRecord(transition.assign))
    {
	handleError(transition.assign, "Invalid assignment expression");
    }

    if (!(transition.assign.getKind() == CONSTANT &&
	  transition.assign.getValue() == 1)
	&& isSideEffectFree(transition.assign))
    {
 	handleWarning(transition.assign, "Expression does not have any effect");
    }
}

void TypeChecker::visitInstance(instance_t &instance)
{
    Interpreter interpreter(system->getConstantValuation());
    interpreter.addValuation(instance.mapping);

    setContextInstantiation();

    map<symbol_t, expression_t>::iterator i = instance.mapping.begin();
    for (;i != instance.mapping.end(); ++i) {
	type_t parameter = i->first.getType();
	expression_t argument = i->second;
	
	annotate(argument);

	// For template instantiation, the argument must be side effect free
	if (!isSideEffectFree(argument)) {
	    handleError(argument, "Argument must be side effect free");
	    continue;
	}

	// We have three ok cases:
	// - Constant reference with computable argument
	// - Reference parameter with unique lhs argument
	// - Value parameter with computable argument
	// If non of the cases is true, then we generate an error
	bool ref = parameter.hasPrefix(prefix::REFERENCE);
	bool constant = parameter.hasPrefix(prefix::CONSTANT);
	bool computable = !argument.dependsOn(persistentVariables.get());
	
	if (!(ref && constant && computable)
	    && !(ref ? isUniqueReference(argument) : computable))
	{
	    handleError(argument, "Incompatible argument");
	    continue;
	}

	checkParameterCompatible(interpreter, parameter, argument);
    }
}

void TypeChecker::visitProperty(expression_t expr)
{
    setContextNone();
    annotate(expr);
    if (!isSideEffectFree(expr)) 
	handleError(expr, "Property must be side effect free");

    if ((expr.getKind() == LEADSTO &&
	 !(isConstraint(expr[0]) && isConstraint(expr[1])))
	|| (expr.getKind() != LEADSTO && !isConstraint(expr)))
    {
	handleError(expr, "Property must be a constraint");
    }
}

void TypeChecker::checkAssignmentExpressionInFunction(expression_t expr)
{
    if (!isInteger(expr) && !isClock(expr) && !isRecord(expr)) {
	handleError(expr, "Invalid expression in function");
    }

//     if (isSideEffectFree(expr)) {
//  	handleWarning(expr, "Expression does not have any effect");
//     }
}

void TypeChecker::checkConditionalExpressionInFunction(expression_t expr)
{
    if (!isInteger(expr)) {
	handleError(expr, "Boolean expected here");
    }
}

void TypeChecker::visitFunction(function_t &fun)
{
    // TODO: Set the correct context
    fun.body->accept(this);
    // TODO: Make sure that last statement is a return statement
}

int32_t TypeChecker::visitEmptyStatement(EmptyStatement *stat)
{
    return 0;
}

int32_t TypeChecker::visitExprStatement(ExprStatement *stat)
{
    annotate(stat->expr);
    checkAssignmentExpressionInFunction(stat->expr);
    return 0;
}

int32_t TypeChecker::visitForStatement(ForStatement *stat)
{
    annotate(stat->init);
    annotate(stat->cond);
    annotate(stat->step);

    checkAssignmentExpressionInFunction(stat->init);
    checkConditionalExpressionInFunction(stat->cond);
    checkAssignmentExpressionInFunction(stat->step);

    return stat->stat->accept(this);
}

int32_t TypeChecker::visitWhileStatement(WhileStatement *stat)
{
    annotate(stat->cond);
    checkConditionalExpressionInFunction(stat->cond);
    return stat->stat->accept(this);
}

int32_t TypeChecker::visitDoWhileStatement(DoWhileStatement *stat)
{
    annotate(stat->cond);
    checkConditionalExpressionInFunction(stat->cond);
    return stat->stat->accept(this);
}

int32_t TypeChecker::visitBlockStatement(BlockStatement *stat)
{
    BlockStatement::iterator i;
    for (i = stat->begin(); i != stat->end(); ++i) {
	(*i)->accept(this);
    }
    return 0;
}

int32_t TypeChecker::visitSwitchStatement(SwitchStatement *stat)
{
    annotate(stat->cond);
    // TODO: Check type of expressions
    return visitBlockStatement(stat);
}

int32_t TypeChecker::visitCaseStatement(CaseStatement *stat)
{
    annotate(stat->cond);
    // TODO: Check type of expressions
    return visitBlockStatement(stat);
}

int32_t TypeChecker::visitDefaultStatement(DefaultStatement *stat)
{
    return visitBlockStatement(stat);
}

int32_t TypeChecker::visitIfStatement(IfStatement *stat)
{
    annotate(stat->cond);
    checkConditionalExpressionInFunction(stat->cond);
    stat->trueCase->accept(this);
    if (stat->falseCase) {
	stat->falseCase->accept(this);
    }
    return 0;
}

int32_t TypeChecker::visitBreakStatement(BreakStatement *stat)
{
    return 0;
}

int32_t TypeChecker::visitContinueStatement(ContinueStatement *stat)
{   
    return 0;
}

int32_t TypeChecker::visitReturnStatement(ReturnStatement *stat)
{
    annotate(stat->value);
    // TODO: Check type of expressions
    return 0;
}

bool TypeChecker::isInteger(expression_t expr) const
{
    return expr.getType().getBase() == type_t::INT
	|| expr.getType().getBase() == type_t::BOOL;
}

bool TypeChecker::isClock(expression_t expr) const
{
    return expr.getType().getBase() == type_t::CLOCK;
}

bool TypeChecker::isRecord(expression_t expr) const
{
    return expr.getType().getBase() == type_t::RECORD;
}

bool TypeChecker::isDiff(expression_t expr) const
{
    return expr.getType().getBase() == type_t::DIFF;
}

/**
   Returns true iff type is a valid invariant. A valid invariant is
   either an invariant expression or an integer expression.
*/
bool TypeChecker::isInvariant(expression_t expr) const
{
    return expr.empty()
	|| (expr.getType().getBase() == type_t::INVARIANT)
	|| isInteger(expr);
}

/**
   Returns true iff type is a valid guard. A valid guard is either a
   valid invariant or a guard expression.
*/
bool TypeChecker::isGuard(expression_t expr) const
{
    return (expr.getType().getBase() == type_t::GUARD) || isInvariant(expr);
}

/**
   Returns true iff type is a valid constraint. A valid constraint is
   either a valid guard or a constraint expression.
*/
bool TypeChecker::isConstraint(expression_t expr) const
{
    return (expr.getType().getBase() == type_t::CONSTRAINT) || isGuard(expr);
}

expression_t TypeChecker::makeConstant(int n)
{
    return expression_t::createConstant(position_t(), n);
}

/** Returns a value indicating the capabilities of a channel. For
    urgent channels this is 0, for non-urgent broadcast channels this
    is 1, and in all other cases 2. An argument to a channel parameter
    must have at least the same capability as the parameter. */
static int channelCapability(type_t type)
{
    assert(type.getBase() == type_t::CHANNEL);
    if (type.hasPrefix(prefix::URGENT))
	return 0;
    if (type.hasPrefix(prefix::BROADCAST))
	return 1;
    return 2;
}

/**
 * Checks whether argument type is compatible with parameter type.
 *
 * REVISIT: The reasoning behind the current implementation is
 * strange. For constant reference parameters, it is ok to specify
 * constant arguments; but these arguments might themself be constant
 * references to non-constant variables. E.g.
 *
 *   void f(const int &i) {}
 *   void g(const int &j) { f(j); }
 *
 * where g() is called with a regular variable. When checking the call
 * of f() in g(), we have that isLHSValue(j) return false (because we
 * cannot assign to j in g()). We then conclude that the call is valid
 * anyway (which is a correct conclusion), because we can always
 * evaluate j and create a temporary variable for i (this is an
 * incorrect argument, because what actually happens is that we pass
 * on the reference we got when g() was called).
 *
 * The current implementation seems to work, but for the wrong
 * reasons!
 */
void TypeChecker::checkParameterCompatible(
    const Interpreter &interpreter, type_t paramType, expression_t arg)
{
    bool ref = paramType.hasPrefix(prefix::REFERENCE);
    bool constant = paramType.hasPrefix(prefix::CONSTANT);
    bool lhs = isLHSValue(arg);

    type_t argType = arg.getType();

    if (!ref) {
	// If the parameter is not a reference, then we can do type
	// conversion between booleans and integers.

	if (paramType.getBase() == type_t::INT
	    && argType.getBase() == type_t::BOOL)
	{
	    argType = type_t::createInteger(
		expression_t::createConstant(position_t(), 0),
		expression_t::createConstant(position_t(), 1));
	    lhs = false;
	}

	if (paramType.getBase() == type_t::BOOL
	    && argType.getBase() == type_t::INT)
	{
	    argType = type_t::BOOL;
	    lhs = false;
	}
    }
    
    // For non-const reference parameters, we require a lhs argument
    if (ref && !constant && !lhs)
    {
	handleError(arg, "Reference parameter requires left value argument");
	return;
    }
    
    // Resolve base type of arrays
    while (paramType.getBase() == type_t::ARRAY) {
	if (argType.getBase() != type_t::ARRAY) {
	    handleError(arg, "Incompatible argument to array parameter");
	    return;
	}

	try {
	    int32_t argSize = interpreter.evaluate(argType.getArraySize());
	    int32_t paramSize = interpreter.evaluate(paramType.getArraySize());
	
	    if (argSize != paramSize)
		handleError(arg, "Parameter array size does not match argument array size");
	} catch (InterpreterException) {
	    assert(0);
	}
	    
	paramType = paramType.getSub();
	argType = argType.getSub();
    }

    // The parameter and the argument must have the same base type
    if (paramType.getBase() != argType.getBase()) {
	handleError(arg, "Incompatible argument");
	return;
    }

    type_t base = paramType.getBase();
    if (base == type_t::CLOCK || base == type_t::BOOL) {
	// For clocks and booleans there is no more to check
	return;
    }

    if (base == type_t::INT) {
	// For integers we need to consider the range: The main
	// purpose is to ensure that arguments to reference parameters
	// are within range of the parameter. For non-reference
	// parameters we still try to check whether the argument is
	// outside the range of the parameter, but this can only be
	// done if the argument is computable at parse time.

	// Special case; if parameter has no range, then everything
	// is accepted - this ensures compatibility with 3.2
	if (paramType.getRange().first.empty())
	    return;

	// There are two main cases
	//
	// case a: if we have a left value argument, then there is no
	// way we can compute the exact value of the argument. In this
	// case we must use the declared range.
	//
	// case b: if it is not a left value argument, then we might
	// be able to compute the exact value, which is what we will
	// try to do.
	
	if (lhs) {
	    // case a
	    try {
		// First try to compute the declared range of the
		// argument and the paramter.
		range_t paramRange = interpreter.evaluate(paramType.getRange());
		range_t argRange = interpreter.evaluate(argType.getRange());

		// For non-constant reference parameters the argument
		// range must match that of the parameter.
		if (ref && !constant && argRange != paramRange) {
		    handleError(arg, "Range of argument does not match range of formal parameter");
		    return;
		}

		// For constant reference parameters the argument
		// range must be contained in the paramtere range.
		if (ref && constant && !paramRange.contains(argRange)) {
		    handleError(arg, "Range of argument is outside of the range of the formal parameter");
		    return;
		}

		// In case the two ranges do not intersect at all,
		// then the argument can never be valid.
		if (paramRange.intersect(argRange).isEmpty()) {
		    handleError(arg, "Range of argument is outside of the range of the formal parameter");
		    return;
		}
	    } catch (InterpreterException) {
		// Computing the declared range failed.

		if (ref) {
		    // For reference parameters we check that the
		    // range declaration of the argument is identical
		    // to that of the parameter.
		    pair<expression_t, expression_t> paramRange, argRange;
		    paramRange = paramType.getRange();
		    argRange = argType.getRange();
		    if (!paramRange.first.equal(argRange.first) ||
			!paramRange.second.equal(argRange.second))
		    {
			handleError(arg, "Range of argument does not match range of formal parameter");
		    }
		}
	    }
	} else {
	    // case b
	    try {
		range_t argRange, paramRange;

		paramRange = interpreter.evaluate(paramType.getRange());

		vector<int32_t> value;
		interpreter.evaluate(arg, value);
		for (uint32_t i = 0; i < value.size(); i++) {
		    argRange = argRange.join(range_t(value[i]));
		}
		
		if (!paramRange.contains(argRange)) {
		    handleError(arg, "Range of argument is outside of the range of the formal parameter");	    
		}
	    } catch (InterpreterException) {
		// Bad luck: we need to revert to runtime checking 
	    }
	}
    } else if (base == type_t::RECORD) {
	if (paramType.getRecordFields() != argType.getRecordFields()) 
	    handleError(arg, "Argument has incompatible type");
    } else if (base == type_t::CHANNEL) {
	if (channelCapability(argType) < channelCapability(paramType))
	    handleError(arg, "Incompatible channel type");
    } else {
	assert(false);
    }
}

/** Checks whether init is a valid initialiser for a variable or
    constant of the given type. */
void TypeChecker::checkInitialiser(type_t type, expression_t init)
{
    Interpreter interpreter(system->getConstantValuation());
    type_t base = type.getBase();
    if (base == type_t::ARRAY) {
        if (init.getKind() != LIST)
            throw InitialiserException(init, "Invalid array initialiser");
	
	int32_t dim;
	try {
	    dim = interpreter.evaluate(type.getArraySize());
	} catch (InterpreterException) {
	    throw InitialiserException(
		init, "Arrays with parameterized size cannot have an initialiser");
	}

        if (init.getSize() > (uint32_t)dim)
            throw InitialiserException(init,
				       "Excess elements in array initialiser");

	type_t subtype = type.getSub();
	frame_t fields = init.getType().getRecordFields();
        for (uint32_t i = 0; i < fields.getSize(); i++) {
            if (fields[i].getName() != NULL) 
		throw InitialiserException(
		    init[i], "Unknown field specified in initialiser");
            checkInitialiser(subtype, init[i]);
        }
        
        if (fields.getSize() < (uint32_t)dim) 
	    throw InitialiserException(init, "Missing fields in initialiser");
    } else if (base == type_t::BOOL) {
	if (!isInteger(init)) {
	    throw InitialiserException(init, "Invalid initialiser");
	}
    } else if (base == type_t::INT) {
	if (!isInteger(init)) {
	    throw InitialiserException(init, "Invalid initialiser");
	}

	// If there is no range (this might be the case when the
	// variable is a constant), then we cannot do anymore.
	if (type.getRange().first.empty()) {
	    return;
	}

	// In general we cannot assure that the initialiser is within
	// the range of the variable - what we can do is to check that
	// if both the range of the variable and the initialiser are
	// computable, then the initialiser should be within the
	// range.

	try {
	    // If possible, compute value and range
	    int n = interpreter.evaluate(init);
	    range_t range = interpreter.evaluate(type.getRange());
	
	    // YES! Everything was computable, so make sure that initialiser
	    // is within range.
	    if (!range.contains(n))
		throw InitialiserException(init, "Initialiser is out of range");
	} catch (InterpreterException) {
	    // NO! We cannot check more.
	    return;
	}
    } else if (base == type_t::RECORD) {
	if (type.getRecordFields() == init.getType().getRecordFields())
	    return;

	if (init.getKind() != LIST) 
	    throw InitialiserException(init, "Invalid initialiser for struct");

	frame_t fields = type.getRecordFields();
	frame_t initialisers = init.getType().getRecordFields();
	vector<bool> hasInitialiser(fields.getSize(), false);

	int32_t current = 0;
	for (uint32_t i = 0; i < initialisers.getSize(); i++, current++) {
	    if (initialisers[i].getName() != NULL) {
		current = fields.getIndexOf(initialisers[i].getName());
		if (current == -1) {
		    handleError(init[i], "Unknown field");
		    break;
		}
	    }

	    if (current >= (int32_t)fields.getSize()) {
		handleError(init[i], "Excess elements in intialiser");
		break;
	    }
	    
	    if (hasInitialiser[current]) {
		handleError(init[i], "Multiple initialisers for field");
		continue;
	    }
	    
	    hasInitialiser[current] = true;
	    checkInitialiser(fields[current].getType(), init[i]);
	}

	// Check the type of each initialiser and that all fields do
	// have an initialiser.
	for (uint32_t i = 0; i < fields.getSize(); i++) {
	    if (!hasInitialiser[i]) {
		throw InitialiserException(init, "Incomplete initialiser");
	    }	
	}
    }
}

/** Checks the initialiser of a constant or a variable */
void TypeChecker::checkInitialiser(variable_t &var)
{
    try 
    {
	if (!var.expr.empty()) 
	{
	    annotate(var.expr);
	    if (var.expr.dependsOn(persistentVariables.get())) 
	    {
		handleError(var.expr, "Constant expression expected");
	    }
	    else if (!isSideEffectFree(var.expr)) 
	    {
		handleError(var.expr, "Initialiser must not have side effects");
	    } 
	    else 
	    {
		checkInitialiser(var.uid.getType(), var.expr);
	    }
	}
    } 
    catch (InitialiserException &e) 
    {
	handleError(e.getExpression(), e.what());
    }
}

/** Returns the type of a binary operation with non-integer operands. */
type_t TypeChecker::typeOfBinaryNonInt(
    expression_t left, uint32_t binaryop, expression_t right)
{
    type_t type;
    
    switch (binaryop) {
    case PLUS:
	if (isInteger(left) && isClock(right)
	    || isClock(left) && isInteger(right))
	{
	    type = type_t::CLOCK;
	} else if (isDiff(left) && isInteger(right) 
		   || isInteger(left) && isDiff(right))
	{
	    type = type_t::DIFF;
	}
	break;
	    
    case MINUS:
	if (isClock(left) && isInteger(right)) 
	    // removed  "|| isInteger(left.type) && isClock(right.type)" 
	    // in order to be able to convert into ClockGuards
	{
	    type = type_t::CLOCK;
	} 
	else if (isDiff(left) && isInteger(right)
		 || isInteger(left) && isDiff(right)
		 || isClock(left) && isClock(right)) 
	{
	    type = type_t::DIFF;
	}
	break;

    case AND:
	if (isInvariant(left) && isInvariant(right)) {
	    type = type_t::INVARIANT;
	} else if (isGuard(left) && isGuard(right)) {
	    type = type_t::GUARD;
	} else if (isConstraint(left) && isConstraint(right)) {
	    type = type_t::CONSTRAINT;
	}
	break;
	    
    case OR:
	if (isConstraint(left) && isConstraint(right)) {
	    type = type_t::CONSTRAINT;
	}
	break;

    case LT:
    case LE:
	if (isClock(left) && isClock(right)
	    || isClock(left) && isInteger(right)
	    || isDiff(left) && isInteger(right)
	    || isInteger(left) && isDiff(right))
	{
	    type = type_t::INVARIANT;
	} else if (isInteger(left) && isClock(right)) {
	    type = type_t::GUARD;
	}
	break;

    case EQ:
	if (isClock(left) && isClock(right)
	    || isClock(left) && isInteger(right)
	    || isInteger(left) && isClock(right)
	    || isDiff(left) && isInteger(right)
	    || isInteger(left) && isDiff(right))
	{
	    type = type_t::GUARD;
	}
	break;
	
    case NEQ:
	if (isClock(left) && isClock(right)
	    || isClock(left) && isInteger(right)
	    || isInteger(left) && isClock(right)
	    || isDiff(left) && isInteger(right)
	    || isInteger(left) && isDiff(right))
	{
	    type = type_t::CONSTRAINT;
	}
	break;

    case GE:
    case GT:
	if (isClock(left) && isClock(right)
	    || isInteger(left) && isClock(right)
	    || isDiff(left) && isInteger(right)
	    || isInteger(left) && isDiff(right))
	{
	    type = type_t::INVARIANT;
	} else if (isClock(left) && isGuard(right)) {
	    type = type_t::GUARD;
	}
	break;
    }

    return type;
}

/** Returns true if arguments of an inline if are compatible.  Clocks
    are only compatible with clocks, integers and booleans are
    compatible, channels are only compatible with channels with
    identical prefixes. Arrays must have the same size and the
    subtypes must be compatible. Records must have the same type name.
*/
bool TypeChecker::areInlineIfCompatible(type_t thenArg, type_t elseArg)
{
    type_t thenBase = thenArg.getBase();
    type_t elseBase = elseArg.getBase();
    if (thenBase == type_t::INT || thenBase == type_t::BOOL)
    {
	return elseBase == type_t::INT || elseBase == type_t::BOOL;
    }
    else if (thenBase == type_t::CLOCK)
    {
	return elseBase == type_t::CLOCK;
    }
    else if (thenBase == type_t::CHANNEL)
    {
	return elseBase == type_t::CHANNEL
	    && (thenArg.hasPrefix(prefix::URGENT) 
		== elseArg.hasPrefix(prefix::URGENT))
	    && (thenArg.hasPrefix(prefix::BROADCAST) 
		== elseArg.hasPrefix(prefix::BROADCAST));
    }
    else if (thenBase == type_t::ARRAY)
    {
	return elseBase == type_t::ARRAY
	    && thenArg.getArraySize().equal(elseArg.getArraySize())
	    && areInlineIfCompatible(thenArg.getSub(), elseArg.getSub());
    }
    else if (thenBase == type_t::RECORD)
    {
	return elseBase == type_t::RECORD
	    && thenArg.getRecordFields() == elseArg.getRecordFields();
    }

    return false;
}

/** Returns true if lvalue and rvalue are assignment compatible.  This
    is the case when an expression of type rvalue can be assigned to
    an expression of type lvalue. It does not check whether lvalue is
    actually a left-hand side value. In case of integers, it does not
    check the range of the expressions.
*/
bool TypeChecker::areAssignmentCompatible(type_t lvalue, type_t rvalue) 
{
    type_t lbase = lvalue.getBase();
    type_t rbase = rvalue.getBase();

    if (lbase == type_t::VOID_TYPE) {
	return false;
    }

    if (lbase == type_t::CLOCK || lbase == type_t::INT || lbase == type_t::BOOL) {
	return (rbase == type_t::INT || rbase == type_t::BOOL);
    }

    if (lbase == type_t::RECORD) {
	return (rvalue.getBase() == type_t::RECORD
		&& lvalue.getRecordFields() != rvalue.getRecordFields());
    }

    return false;
}

void TypeChecker::checkFunctionCallArguments(expression_t expr)
{
    // REVISIT: We don't know anything about the context of this
    // expression, but the additional mapping provided by the context
    // might be important additions to the interpreter. In particular,
    // it might be necessary to add the parameter mapping from the
    // call itself. E.g. consider a function
    //
    //  int f(const int N, int a[N])
    //
    // Here it is important to know N when checking the second
    // argument. At the moment this is not allowed by the
    // SystemBuilder, though.

    type_t type = expr[0].getType();
    frame_t parameters = type.getParameters();

    if (parameters.getSize() > expr.getSize() - 1) 
    {
	handleError(expr, "Too few arguments");
    }
    else if (parameters.getSize() < expr.getSize() - 1)
    {
	for (uint32_t i = parameters.getSize() + 1; i < expr.getSize(); i++)
	{
	    handleError(expr[i], "Too many arguments");
	}
    }
    else
    {
	Interpreter interpreter(system->getConstantValuation());
	for (uint32_t i = 0; i < parameters.getSize(); i++) 
	{
	    type_t parameter = parameters[i].getType();
	    expression_t argument = expr[i + 1];
	    checkParameterCompatible(interpreter, parameter, argument);
	}
    }
}

/** Type check and annotate the expression. This function performs
    basic type checking of the given expression and assigns a type to
    every subexpression of the expression. It checks that only
    left-hand side values are updated, checks that functions are
    called with the correct arguments, checks that operators are used
    with the correct operands and checks that operands to assignment
    operators are assignment compatible. Errors are reported by
    calling handleError(). This function does not check/compute the
    range of integer expressions and thus does not produce
    out-of-range errors or warnings.
*/
void TypeChecker::annotate(expression_t expr)
{
    if (expr.empty())
	return;

    for (uint32_t i = 0; i < expr.getSize(); i++) 
	annotate(expr[i]);

    type_t type, arg1, arg2, arg3;
    switch (expr.getKind()) {
    case EQ:
    case NEQ:
	if (isInteger(expr[0]) && isInteger(expr[1])) {
	    type = type_t::INT;
	} else if (expr[0].getType().getBase() == type_t::RECORD
		   && expr[0].getType().getRecordFields() 
		   == expr[1].getType().getRecordFields())
	{
	    type = type_t::INT;
	} else {
	    type = typeOfBinaryNonInt(expr[0], expr.getKind(), expr[1]);
	    if (type == type_t()) {
		handleError(expr, "Invalid operands to binary operator");
		type = type_t::CONSTRAINT;
	    }
	}
	break;

    case PLUS:
    case MINUS:
    case MULT:
    case DIV:
    case MOD:
    case BIT_AND:
    case BIT_OR:
    case BIT_XOR:
    case BIT_LSHIFT:
    case BIT_RSHIFT:
    case AND:
    case OR:
    case MIN:
    case MAX:
    case LT:
    case LE:
    case GE:
    case GT:
	if (isInteger(expr[0]) && isInteger(expr[1])) {
	    type = type_t::INT;
	} else {
	    type = typeOfBinaryNonInt(expr[0], expr.getKind(), expr[1]);
	    if (type == type_t()) {
		handleError(expr, "Invalid operands to binary operator");
		type = type_t::CONSTRAINT;
	    }
	}
	break;

    case NOT:
	if (isInteger(expr[0])) {
	    type = type_t::INT;
	} else if (isConstraint(expr[0])) {
	    type = type_t::CONSTRAINT;
	} else {
	    handleError(expr, "Invalid operation for type");
	    type = type_t::INT;
	}
	break;
	
    case UNARY_MINUS:
	if (!isInteger(expr[0])) {
	    handleError(expr, "Invalid operation for type");
	}
	type = type_t::INT;
	break;

    case ASSIGN:
	if (!areAssignmentCompatible(expr[0].getType(), expr[1].getType())) {
	    handleError(expr, "Incompatible types");
	} else if (!isLHSValue(expr[0])) {
	    handleError(expr[0], "Left hand side value expected");
	}
	type = expr[0].getType();
	break;
      
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
	if (!isInteger(expr[0]) || !isInteger(expr[1])) {
	    handleError(expr, "Non-integer types must use regular assignment operator.");
	} else if (!isLHSValue(expr[0])) {
	    handleError(expr[0], "Left hand side value expected");	    
	}
	type = expr[0].getType();
	break;
      
    case POSTINCREMENT:
    case PREINCREMENT:
    case POSTDECREMENT:
    case PREDECREMENT:
	if (expr[0].getType().getBase() != type_t::INT) {
	    handleError(expr, "Argument must be an integer value");
	} else if (!isLHSValue(expr[0])) {
	    handleError(expr[0], "Left hand side value expected");	    
	}
	type = type_t::INT;
	break;
    
    case INLINEIF:
	if (!isInteger(expr[0])) {
	    handleError(expr, "First argument of inline if must be an integer");
	}
	if (!areInlineIfCompatible(expr[1].getType(), expr[2].getType())) {
	    handleError(expr, "Incompatible arguments to inline if");
	}
	type = expr[1].getType();
	break;
      
    case COMMA:
	if (!isInteger(expr[0]) && !isClock(expr[0]) && !isRecord(expr[0])
	    || !isInteger(expr[1]) && !isClock(expr[1]) && !isRecord(expr[1]))
	{
	    handleError(expr, "Arguments must be of integer, clock or record type");
	}
	type = expr[1].getType();
	break;
      
    case FUNCALL:
	if (expr[0].getType().getBase() != type_t::FUNCTION)
	{
	    handleError(expr[0], "A function name was expected here");
	}
	else
	{
	    checkFunctionCallArguments(expr);
	}
	return;
      
    default:
	return;
    }
    expr.setType(type);
}

/** Returns true if the expression is side effect free. An expression
    is side effect free if it does not modify any variables except
    variables local to functions (and thus not part of the variable
    vector).
*/
bool TypeChecker::isSideEffectFree(expression_t expr) const
{
    return !expr.changesVariable(persistentVariables.get());
}

/** Returns true if expression is a left-hand-side value.
    Left-hand-side values are expressions that result in references to
    variables. Note: An inline if over integers is only a LHS value if
    both results have the same declared range.
*/
bool TypeChecker::isLHSValue(expression_t expr) const
{
    type_t t, f;
    switch (expr.getKind()) {
    case IDENTIFIER:
	return !expr.getSymbol().getType().hasPrefix(prefix::CONSTANT);

    case DOT:
    case ARRAY:
	// REVISIT: What if expr[0] is a process?
	return isLHSValue(expr[0]);
	
    case PREINCREMENT:
    case PREDECREMENT:
    case ASSIGN:
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
	return isLHSValue(expr[0]);	    // REVISIT: Maybe skip this
	
    case INLINEIF:
	if (!isLHSValue(expr[1]) || !isLHSValue(expr[2]))
	    return false;
	
	// The annotate() method ensures that the value of the two
	// result arguments are compatible; for integers we
	// additionally require them to have the same (syntactic)
	// range declaration for them to be usable as LHS values.

	t = expr[1].getSymbol().getType();
	f = expr[2].getSymbol().getType();

	while (t.getBase() == type_t::ARRAY) t = t.getSub();
	while (f.getBase() == type_t::ARRAY) f = f.getSub();

	return (t.getBase() != type_t::INT 
		|| (t.getRange().first.equal(f.getRange().first)
		    && t.getRange().second.equal(f.getRange().second)));
      
    case COMMA:
	return isLHSValue(expr[1]);

    case FUNCALL:
	// Functions cannot return references (yet!)

    default:
	return false;
    }
}

/** Returns true if expression is a reference to a unique variable.
    Thus is similar to expr being an LHS value, but in addition we
    require that the reference does not depend on any non-computable
    expressions. Thus i[v] is a LHS value, but if v is a non-constant
    variable, then it does not result in a unique reference.
*/
bool TypeChecker::isUniqueReference(expression_t expr) const
{
    switch (expr.getKind()) {
    case IDENTIFIER:
	return !expr.getType().hasPrefix(prefix::CONSTANT);
    case DOT:
	return isUniqueReference(expr[0]);

    case ARRAY:
	return isUniqueReference(expr[0])
	    && !expr[1].dependsOn(persistentVariables.get());
	
    case PREINCREMENT:
    case PREDECREMENT:
    case ASSIGN:
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
	return isUniqueReference(expr[0]);
	
    case INLINEIF:
	return false;
      
    case COMMA:
	return isUniqueReference(expr[1]);

    case FUNCALL:
	// Functions cannot return references (yet!)

    default:
	return false;
    }
}


bool parseXTA(FILE *file, ErrorHandler *error, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    TypeChecker checker(error);
    parseXTA(file, &builder, error, newxta);
    system->accept(checker);
    return !error->hasErrors();
}

bool parseXTA(const char *buffer, ErrorHandler *error, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    TypeChecker checker(error);
    parseXTA(buffer, &builder, error, newxta);
    system->accept(checker);
    return !error->hasErrors();
}

bool parseXMLBuffer(const char *buffer, ErrorHandler *error, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    TypeChecker checker(error);
    parseXMLBuffer(buffer, &builder, error, newxta);
    system->accept(checker);
    return !error->hasErrors();
}

bool parseXMLFile(const char *file, ErrorHandler *error, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    TypeChecker checker(error);
    parseXMLFile(file, &builder, error, newxta);
    system->accept(checker);
    return !error->hasErrors();
}

