/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2018 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


//---------------------------------------------------------------------------
#include "checkuninitvar.h"

#include "astutils.h"
#include "checknullpointer.h"   // CheckNullPointer::isPointerDeref
#include "errorlogger.h"
#include "library.h"
#include "mathlib.h"
#include "settings.h"
#include "symboldatabase.h"
#include "token.h"
#include "tokenize.h"
#include "valueflow.h"

#include <tinyxml2.h>

#include <cassert>
#include <cstddef>
#include <list>
#include <map>
#include <stack>
#include <utility>
//---------------------------------------------------------------------------

// Register this check class (by creating a static instance of it)
namespace {
    CheckUninitVar instance;
}

//---------------------------------------------------------------------------

// CWE ids used:
static const struct CWE CWE476(476U);  // NULL Pointer Dereference
static const struct CWE CWE676(676U);
static const struct CWE CWE908(908U);
static const struct CWE CWE825(825U);

void CheckUninitVar::check()
{
    const SymbolDatabase *symbolDatabase = mTokenizer->getSymbolDatabase();

    std::set<std::string> arrayTypeDefs;
    for (const Token *tok = mTokenizer->tokens(); tok; tok = tok->next()) {
        if (Token::Match(tok, "%name% [") && tok->variable() && Token::Match(tok->variable()->typeStartToken(), "%type% %var% ;"))
            arrayTypeDefs.insert(tok->variable()->typeStartToken()->str());
    }

    // check every executable scope
    for (const Scope &scope : symbolDatabase->scopeList) {
        if (scope.isExecutable()) {
            checkScope(&scope, arrayTypeDefs);
        }
    }
}

void CheckUninitVar::checkScope(const Scope* scope, const std::set<std::string> &arrayTypeDefs)
{
    for (const Variable &var : scope->varlist) {
        if ((mTokenizer->isCPP() && var.type() && !var.isPointer() && var.type()->needInitialization != Type::True) ||
            var.isStatic() || var.isExtern() || var.isReference())
            continue;

        // don't warn for try/catch exception variable
        if (var.isThrow())
            continue;

        if (Token::Match(var.nameToken()->next(), "[({:]"))
            continue;

        if (Token::Match(var.nameToken(), "%name% =")) { // Variable is initialized, but Rhs might be not
            checkRhs(var.nameToken(), var, NO_ALLOC, 0U, emptyString);
            continue;
        }
        if (Token::Match(var.nameToken(), "%name% ) (") && Token::simpleMatch(var.nameToken()->linkAt(2), ") =")) { // Function pointer is initialized, but Rhs might be not
            checkRhs(var.nameToken()->linkAt(2)->next(), var, NO_ALLOC, 0U, emptyString);
            continue;
        }

        if (var.isArray() || var.isPointerToArray()) {
            const Token *tok = var.nameToken()->next();
            if (var.isPointerToArray())
                tok = tok->next();
            while (Token::simpleMatch(tok->link(), "] ["))
                tok = tok->link()->next();
            if (Token::Match(tok->link(), "] =|{"))
                continue;
        }

        bool stdtype = mTokenizer->isC() && arrayTypeDefs.find(var.typeStartToken()->str()) == arrayTypeDefs.end();
        const Token* tok = var.typeStartToken();
        for (; tok != var.nameToken() && tok->str() != "<"; tok = tok->next()) {
            if (tok->isStandardType() || tok->isEnumType())
                stdtype = true;
        }
        if (var.isArray() && !stdtype)
            continue;

        while (tok && tok->str() != ";")
            tok = tok->next();
        if (!tok)
            continue;

        if (tok->astParent() && Token::simpleMatch(tok->astParent()->previous(), "for (") &&
            checkLoopBody(tok->astParent()->link()->next(), var, var.isArray() ? ARRAY : NO_ALLOC, emptyString, true))
            continue;

        if (var.isArray()) {
            Alloc alloc = ARRAY;
            const std::map<unsigned int, VariableValue> variableValue;
            checkScopeForVariable(tok, var, nullptr, nullptr, &alloc, emptyString, variableValue);
            continue;
        }
        if (stdtype || var.isPointer()) {
            Alloc alloc = NO_ALLOC;
            const std::map<unsigned int, VariableValue> variableValue;
            checkScopeForVariable(tok, var, nullptr, nullptr, &alloc, emptyString, variableValue);
        }
        if (var.type())
            checkStruct(tok, var);
    }

    if (scope->function) {
        for (const Variable &arg : scope->function->argumentList) {
            if (arg.declarationId() && Token::Match(arg.typeStartToken(), "%type% * %name% [,)]")) {
                // Treat the pointer as initialized until it is assigned by malloc
                for (const Token *tok = scope->bodyStart; tok != scope->bodyEnd; tok = tok->next()) {
                    if (Token::Match(tok, "[;{}] %varid% = %name% (", arg.declarationId()) &&
                        mSettings->library.returnuninitdata.count(tok->strAt(3)) == 1U) {
                        if (arg.typeStartToken()->strAt(-1) == "struct" || (arg.type() && arg.type()->isStructType()))
                            checkStruct(tok, arg);
                        else if (arg.typeStartToken()->isStandardType() || arg.typeStartToken()->isEnumType()) {
                            Alloc alloc = NO_ALLOC;
                            const std::map<unsigned int, VariableValue> variableValue;
                            checkScopeForVariable(tok->next(), arg, nullptr, nullptr, &alloc, emptyString, variableValue);
                        }
                    }
                }
            }
        }
    }
}

void CheckUninitVar::checkStruct(const Token *tok, const Variable &structvar)
{
    const Token *typeToken = structvar.typeStartToken();
    const SymbolDatabase * symbolDatabase = mTokenizer->getSymbolDatabase();
    for (const Scope *scope2 : symbolDatabase->classAndStructScopes) {
        if (scope2->className == typeToken->str() && scope2->numConstructors == 0U) {
            for (const Variable &var : scope2->varlist) {
                if (var.isStatic() || var.hasDefault() || var.isArray() ||
                    (!mTokenizer->isC() && var.isClass() && (!var.type() || var.type()->needInitialization != Type::True)))
                    continue;

                // is the variable declared in a inner union?
                bool innerunion = false;
                for (const Scope *innerScope : scope2->nestedList) {
                    if (innerScope->type == Scope::eUnion) {
                        if (var.typeStartToken()->linenr() >= innerScope->bodyStart->linenr() &&
                            var.typeStartToken()->linenr() <= innerScope->bodyEnd->linenr()) {
                            innerunion = true;
                            break;
                        }
                    }
                }

                if (!innerunion) {
                    Alloc alloc = NO_ALLOC;
                    const Token *tok2 = tok;
                    if (tok->str() == "}")
                        tok2 = tok2->next();
                    const std::map<unsigned int, VariableValue> variableValue;
                    checkScopeForVariable(tok2, structvar, nullptr, nullptr, &alloc, var.name(), variableValue);
                }
            }
        }
    }
}

static VariableValue operator!(VariableValue v)
{
    v.notEqual = !v.notEqual;
    return v;
}
static bool operator==(const VariableValue & v, MathLib::bigint i)
{
    return v.notEqual ? (i != v.value) : (i == v.value);
}
static bool operator!=(const VariableValue & v, MathLib::bigint i)
{
    return v.notEqual ? (i == v.value) : (i != v.value);
}

static void conditionAlwaysTrueOrFalse(const Token *tok, const std::map<unsigned int, VariableValue> &variableValue, bool *alwaysTrue, bool *alwaysFalse)
{
    if (!tok)
        return;

    if (tok->isName() || tok->str() == ".") {
        while (tok && tok->str() == ".")
            tok = tok->astOperand2();
        const std::map<unsigned int, VariableValue>::const_iterator it = variableValue.find(tok ? tok->varId() : ~0U);
        if (it != variableValue.end()) {
            *alwaysTrue = (it->second != 0LL);
            *alwaysFalse = (it->second == 0LL);
        }
    }

    else if (tok->isComparisonOp()) {
        if (tok->hasKnownIntValue()) {
            if (tok->values().front().intvalue)
                *alwaysTrue = true;
            else
                *alwaysFalse = true;
            return;
        }

        const Token *vartok, *numtok;
        if (tok->astOperand2() && tok->astOperand2()->isNumber()) {
            vartok = tok->astOperand1();
            numtok = tok->astOperand2();
        } else if (tok->astOperand1() && tok->astOperand1()->isNumber()) {
            vartok = tok->astOperand2();
            numtok = tok->astOperand1();
        } else {
            return;
        }

        while (vartok && vartok->str() == ".")
            vartok = vartok->astOperand2();

        const std::map<unsigned int, VariableValue>::const_iterator it = variableValue.find(vartok ? vartok->varId() : ~0U);
        if (it == variableValue.end())
            return;

        if (tok->str() == "==")
            *alwaysTrue  = (it->second == MathLib::toLongNumber(numtok->str()));
        else if (tok->str() == "!=")
            *alwaysTrue  = (it->second != MathLib::toLongNumber(numtok->str()));
        else
            return;
        *alwaysFalse = !(*alwaysTrue);
    }

    else if (tok->str() == "!") {
        bool t=false,f=false;
        conditionAlwaysTrueOrFalse(tok->astOperand1(), variableValue, &t, &f);
        if (t||f) {
            *alwaysTrue = !t;
            *alwaysFalse = !f;
        }
    }

    else if (tok->str() == "||") {
        bool t1=false, f1=false;
        conditionAlwaysTrueOrFalse(tok->astOperand1(), variableValue, &t1, &f1);
        bool t2=false, f2=false;
        if (!t1)
            conditionAlwaysTrueOrFalse(tok->astOperand2(), variableValue, &t2, &f2);
        *alwaysTrue = (t1 || t2);
        *alwaysFalse = (f1 && f2);
    }

    else if (tok->str() == "&&") {
        bool t1=false, f1=false;
        conditionAlwaysTrueOrFalse(tok->astOperand1(), variableValue, &t1, &f1);
        bool t2=false, f2=false;
        if (!f1)
            conditionAlwaysTrueOrFalse(tok->astOperand2(), variableValue, &t2, &f2);
        *alwaysTrue = (t1 && t2);
        *alwaysFalse = (f1 || f2);
    }
}

static bool isVariableUsed(const Token *tok, const Variable& var)
{
    if (!tok)
        return false;
    if (tok->str() == "&" && !tok->astOperand2())
        return false;
    if (tok->isConstOp())
        return isVariableUsed(tok->astOperand1(),var) || isVariableUsed(tok->astOperand2(),var);
    if (tok->varId() != var.declarationId())
        return false;
    if (!var.isArray())
        return true;

    const Token *parent = tok->astParent();
    while (Token::Match(parent, "[?:]"))
        parent = parent->astParent();
    // no dereference, then array is not "used"
    if (!Token::Match(parent, "*|["))
        return false;
    const Token *parent2 = parent->astParent();
    // TODO: handle function calls. There is a TODO assertion in TestUninitVar::uninitvar_arrays
    return !parent2 || parent2->isConstOp() || (parent2->str() == "=" && parent2->astOperand2() == parent);
}

bool CheckUninitVar::checkScopeForVariable(const Token *tok, const Variable& var, bool * const possibleInit, bool * const noreturn, Alloc* const alloc, const std::string &membervar, std::map<unsigned int, VariableValue> variableValue)
{
    const bool suppressErrors(possibleInit && *possibleInit);  // Assume that this is a variable delaratkon, rather than a fundef
    const bool printDebug = mSettings->debugwarnings;

    if (possibleInit)
        *possibleInit = false;

    unsigned int number_of_if = 0;

    if (var.declarationId() == 0U)
        return true;

    for (; tok; tok = tok->next()) {
        // End of scope..
        if (tok->str() == "}") {
            if (number_of_if && possibleInit)
                *possibleInit = true;

            // might be a noreturn function..
            if (mTokenizer->IsScopeNoReturn(tok)) {
                if (noreturn)
                    *noreturn = true;
                return false;
            }

            break;
        }

        // Unconditional inner scope or try..
        if (tok->str() == "{" && Token::Match(tok->previous(), ",|;|{|}|try")) {
            if (checkScopeForVariable(tok->next(), var, possibleInit, noreturn, alloc, membervar, variableValue))
                return true;
            tok = tok->link();
            continue;
        }

        // assignment with nonzero constant..
        if (Token::Match(tok->previous(), "[;{}] %var% = - %name% ;"))
            variableValue[tok->varId()] = !VariableValue(0);

        // Inner scope..
        else if (Token::simpleMatch(tok, "if (")) {
            bool alwaysTrue = false;
            bool alwaysFalse = false;

            conditionAlwaysTrueOrFalse(tok->next()->astOperand2(), variableValue, &alwaysTrue, &alwaysFalse);

            // initialization / usage in condition..
            if (!alwaysTrue && checkIfForWhileHead(tok->next(), var, suppressErrors, bool(number_of_if == 0), *alloc, membervar))
                return true;

            // checking if a not-zero variable is zero => bail out
            unsigned int condVarId = 0;
            VariableValue condVarValue(0);
            const Token *condVarTok = nullptr;
            if (alwaysFalse)
                ;
            else if (Token::simpleMatch(tok, "if (") &&
                     astIsVariableComparison(tok->next()->astOperand2(), "!=", "0", &condVarTok)) {
                const std::map<unsigned int,VariableValue>::const_iterator it = variableValue.find(condVarTok->varId());
                if (it != variableValue.end() && it->second != 0)
                    return true;   // this scope is not fully analysed => return true
                else {
                    condVarId = condVarTok->varId();
                    condVarValue = !VariableValue(0);
                }
            } else if (Token::simpleMatch(tok, "if (") && Token::Match(tok->next()->astOperand2(), "==|!=")) {
                const Token *condition = tok->next()->astOperand2();
                const Token *lhs = condition->astOperand1();
                const Token *rhs = condition->astOperand2();
                const Token *vartok = rhs && rhs->isNumber() ? lhs : rhs;
                const Token *numtok = rhs && rhs->isNumber() ? rhs : lhs;
                while (Token::simpleMatch(vartok, "."))
                    vartok = vartok->astOperand2();
                if (vartok && vartok->varId() && numtok) {
                    const std::map<unsigned int,VariableValue>::const_iterator it = variableValue.find(vartok->varId());
                    if (it != variableValue.end() && it->second != MathLib::toLongNumber(numtok->str()))
                        return true;   // this scope is not fully analysed => return true
                    else {
                        condVarId = vartok->varId();
                        condVarValue = VariableValue(MathLib::toLongNumber(numtok->str()));
                        if (condition->str() == "!=")
                            condVarValue = !condVarValue;
                    }
                }
            }

            // goto the {
            tok = tok->next()->link()->next();

            if (!tok)
                break;
            if (tok->str() == "{") {
                bool possibleInitIf((!alwaysTrue && number_of_if > 0) || suppressErrors);
                bool noreturnIf = false;
                const bool initif = !alwaysFalse && checkScopeForVariable(tok->next(), var, &possibleInitIf, &noreturnIf, alloc, membervar, variableValue);

                // bail out for such code:
                //    if (a) x=0;    // conditional initialization
                //    if (b) return; // cppcheck doesn't know if b can be false when a is false.
                //    x++;           // it's possible x is always initialized
                if (!alwaysTrue && noreturnIf && number_of_if > 0) {
                    if (printDebug) {
                        std::string condition;
                        for (const Token *tok2 = tok->linkAt(-1); tok2 != tok; tok2 = tok2->next()) {
                            condition += tok2->str();
                            if (tok2->isName() && tok2->next()->isName())
                                condition += ' ';
                        }
                        reportError(tok, Severity::debug, "debug", "bailout uninitialized variable checking for '" + var.name() + "'. can't determine if this condition can be false when previous condition is false: " + condition);
                    }
                    return true;
                }

                if (alwaysTrue && (initif || noreturnIf))
                    return true;

                std::map<unsigned int, VariableValue> varValueIf;
                if (!alwaysFalse && !initif && !noreturnIf) {
                    for (const Token *tok2 = tok; tok2 && tok2 != tok->link(); tok2 = tok2->next()) {
                        if (Token::Match(tok2, "[;{}.] %name% = - %name% ;"))
                            varValueIf[tok2->next()->varId()] = !VariableValue(0);
                        else if (Token::Match(tok2, "[;{}.] %name% = %num% ;"))
                            varValueIf[tok2->next()->varId()] = VariableValue(MathLib::toLongNumber(tok2->strAt(3)));
                    }
                }

                if (initif && condVarId > 0U)
                    variableValue[condVarId] = !condVarValue;

                // goto the }
                tok = tok->link();

                if (!Token::simpleMatch(tok, "} else {")) {
                    if (initif || possibleInitIf) {
                        ++number_of_if;
                        if (number_of_if >= 2)
                            return true;
                    }
                } else {
                    // goto the {
                    tok = tok->tokAt(2);

                    bool possibleInitElse((!alwaysFalse && number_of_if > 0) || suppressErrors);
                    bool noreturnElse = false;
                    const bool initelse = !alwaysTrue && checkScopeForVariable(tok->next(), var, &possibleInitElse, &noreturnElse, alloc, membervar, variableValue);

                    std::map<unsigned int, VariableValue> varValueElse;
                    if (!alwaysTrue && !initelse && !noreturnElse) {
                        for (const Token *tok2 = tok; tok2 && tok2 != tok->link(); tok2 = tok2->next()) {
                            if (Token::Match(tok2, "[;{}.] %var% = - %name% ;"))
                                varValueElse[tok2->next()->varId()] = !VariableValue(0);
                            else if (Token::Match(tok2, "[;{}.] %var% = %num% ;"))
                                varValueElse[tok2->next()->varId()] = VariableValue(MathLib::toLongNumber(tok2->strAt(3)));
                        }
                    }

                    if (initelse && condVarId > 0U && !noreturnIf && !noreturnElse)
                        variableValue[condVarId] = condVarValue;

                    // goto the }
                    tok = tok->link();

                    if ((alwaysFalse || initif || noreturnIf) &&
                        (alwaysTrue || initelse || noreturnElse))
                        return true;

                    if (initif || initelse || possibleInitElse)
                        ++number_of_if;
                    if (!initif && !noreturnIf)
                        variableValue.insert(varValueIf.begin(), varValueIf.end());
                    if (!initelse && !noreturnElse)
                        variableValue.insert(varValueElse.begin(), varValueElse.end());
                }
            }
        }

        // = { .. }
        else if (Token::simpleMatch(tok, "= {")) {
            // end token
            const Token *end = tok->next()->link();

            // If address of variable is taken in the block then bail out
            if (var.isPointer() || var.isArray()) {
                if (Token::findmatch(tok->tokAt(2), "%varid%", end, var.declarationId()))
                    return true;
            } else if (Token::findmatch(tok->tokAt(2), "& %varid%", end, var.declarationId())) {
                return true;
            }

            // Skip block
            tok = end;
            continue;
        }

        // skip sizeof / offsetof
        if (Token::Match(tok, "sizeof|typeof|offsetof|decltype ("))
            tok = tok->next()->link();

        // for/while..
        else if (Token::Match(tok, "for|while (") || Token::simpleMatch(tok, "do {")) {
            const bool forwhile = Token::Match(tok, "for|while (");

            // is variable initialized in for-head?
            if (forwhile && checkIfForWhileHead(tok->next(), var, tok->str() == "for", false, *alloc, membervar))
                return true;

            // goto the {
            const Token *tok2 = forwhile ? tok->next()->link()->next() : tok->next();

            if (tok2 && tok2->str() == "{") {
                const bool init = checkLoopBody(tok2, var, *alloc, membervar, (number_of_if > 0) || suppressErrors);

                // variable is initialized in the loop..
                if (init)
                    return true;

                // is variable used in for-head?
                bool initcond = false;
                if (!suppressErrors) {
                    const Token *startCond = forwhile ? tok->next() : tok->next()->link()->tokAt(2);
                    initcond = checkIfForWhileHead(startCond, var, false, bool(number_of_if == 0), *alloc, membervar);
                }

                // goto "}"
                tok = tok2->link();

                // do-while => goto ")"
                if (!forwhile) {
                    // Assert that the tokens are '} while ('
                    if (!Token::simpleMatch(tok, "} while (")) {
                        if (printDebug)
                            reportError(tok,Severity::debug,"","assertion failed '} while ('");
                        break;
                    }

                    // Goto ')'
                    tok = tok->linkAt(2);

                    if (!tok)
                        // bailout : invalid code / bad tokenizer
                        break;

                    if (initcond)
                        // variable is initialized in while-condition
                        return true;
                }
            }
        }

        // Unknown or unhandled inner scope
        else if (Token::simpleMatch(tok, ") {") || (Token::Match(tok, "%name% {") && tok->str() != "try")) {
            if (tok->str() == "struct" || tok->str() == "union") {
                tok = tok->linkAt(1);
                continue;
            }
            return true;
        }

        // bailout if there is ({
        if (Token::simpleMatch(tok, "( {")) {
            return true;
        }

        // bailout if there is assembler code or setjmp
        if (Token::Match(tok, "asm|setjmp (")) {
            return true;
        }

        // bailout if there is a goto label
        if (Token::Match(tok, "[;{}] %name% :")) {
            return true;
        }

        if (tok->str() == "?") {
            if (!tok->astOperand2())
                return true;
            const bool used1 = isVariableUsed(tok->astOperand2()->astOperand1(), var);
            const bool used0 = isVariableUsed(tok->astOperand2()->astOperand2(), var);
            const bool err = (number_of_if == 0) ? (used1 || used0) : (used1 && used0);
            if (err)
                uninitvarError(tok, var.nameToken()->str(), *alloc);

            // Todo: skip expression if there is no error
            return true;
        }

        if (Token::Match(tok, "return|break|continue|throw|goto")) {
            if (noreturn)
                *noreturn = true;

            tok = tok->next();
            while (tok && tok->str() != ";") {
                // variable is seen..
                if (tok->varId() == var.declarationId()) {
                    if (!membervar.empty()) {
                        if (Token::Match(tok, "%name% . %name% ;|%cop%") && tok->strAt(2) == membervar)
                            uninitStructMemberError(tok, tok->str() + "." + membervar);
                        else
                            return true;
                    }

                    // Use variable
                    else if (!suppressErrors && isVariableUsage(tok, var.isPointer(), *alloc))
                        uninitvarError(tok, tok->str(), *alloc);

                    else
                        // assume that variable is assigned
                        return true;
                }

                else if (Token::Match(tok, "sizeof|typeof|offsetof|decltype ("))
                    tok = tok->linkAt(1);

                else if (tok->str() == "?") {
                    if (!tok->astOperand2())
                        return true;
                    const bool used1 = isVariableUsed(tok->astOperand2()->astOperand1(), var);
                    const bool used0 = isVariableUsed(tok->astOperand2()->astOperand2(), var);
                    const bool err = (number_of_if == 0) ? (used1 || used0) : (used1 && used0);
                    if (err)
                        uninitvarError(tok, var.nameToken()->str(), *alloc);
                    return true;
                }

                tok = tok->next();
            }

            return (noreturn == nullptr);
        }

        // variable is seen..
        if (tok->varId() == var.declarationId()) {
            // calling function that returns uninit data through pointer..
            if (var.isPointer() &&
                Token::Match(tok->next(), "= %name% (") &&
                Token::simpleMatch(tok->linkAt(3), ") ;") &&
                mSettings->library.returnuninitdata.count(tok->strAt(2)) > 0U) {
                *alloc = NO_CTOR_CALL;
                continue;
            }
            if (var.isPointer() && (var.typeStartToken()->isStandardType() || var.typeStartToken()->isEnumType() || (var.type() && var.type()->needInitialization == Type::True)) && Token::simpleMatch(tok->next(), "= new")) {
                *alloc = CTOR_CALL;

                // type has constructor(s)
                if (var.typeScope() && var.typeScope()->numConstructors > 0)
                    return true;

                // standard or enum type: check if new initializes the allocated memory
                if (var.typeStartToken()->isStandardType() || var.typeStartToken()->isEnumType()) {
                    // scalar new with initialization
                    if (Token::Match(tok->next(), "= new %type% ("))
                        return true;

                    // array new
                    if (Token::Match(tok->next(), "= new %type% [")) {
                        const Token* tokClosingBracket=tok->linkAt(4);
                        // array new with initialization
                        if (tokClosingBracket && Token::simpleMatch(tokClosingBracket->next(), "( )"))
                            return true;
                    }
                }

                continue;
            }


            if (!membervar.empty()) {
                if (isMemberVariableAssignment(tok, membervar)) {
                    checkRhs(tok, var, *alloc, number_of_if, membervar);
                    return true;
                }

                if (isMemberVariableUsage(tok, var.isPointer(), *alloc, membervar))
                    uninitStructMemberError(tok, tok->str() + "." + membervar);

                else if (Token::Match(tok->previous(), "[(,] %name% [,)]"))
                    return true;

            } else {
                // Use variable
                if (!suppressErrors && isVariableUsage(tok, var.isPointer(), *alloc))
                    uninitvarError(tok, tok->str(), *alloc);

                else {
                    if (tok->strAt(1) == "=")
                        checkRhs(tok, var, *alloc, number_of_if, emptyString);

                    // assume that variable is assigned
                    return true;
                }
            }
        }
    }

    return false;
}

bool CheckUninitVar::checkIfForWhileHead(const Token *startparentheses, const Variable& var, bool suppressErrors, bool isuninit, Alloc alloc, const std::string &membervar)
{
    const Token * const endpar = startparentheses->link();
    if (Token::Match(startparentheses, "( ! %name% %oror%") && startparentheses->tokAt(2)->getValue(0))
        suppressErrors = true;
    for (const Token *tok = startparentheses->next(); tok && tok != endpar; tok = tok->next()) {
        if (tok->varId() == var.declarationId()) {
            if (Token::Match(tok, "%name% . %name%")) {
                if (membervar.empty())
                    return true;
                if (tok->strAt(2) == membervar) {
                    if (isMemberVariableAssignment(tok, membervar))
                        return true;

                    if (!suppressErrors && isMemberVariableUsage(tok, var.isPointer(), alloc, membervar))
                        uninitStructMemberError(tok, tok->str() + "." + membervar);
                }
                continue;
            }

            if (isVariableUsage(tok, var.isPointer(), alloc)) {
                if (suppressErrors)
                    continue;
                uninitvarError(tok, tok->str(), alloc);
            }
            return true;
        }
        if (Token::Match(tok, "sizeof|decltype|offsetof ("))
            tok = tok->next()->link();
        if ((!isuninit || !membervar.empty()) && tok->str() == "&&")
            suppressErrors = true;
    }
    return false;
}

bool CheckUninitVar::checkLoopBody(const Token *tok, const Variable& var, const Alloc alloc, const std::string &membervar, const bool suppressErrors)
{
    const Token *usetok = nullptr;

    assert(tok->str() == "{");

    for (const Token * const end = tok->link(); tok != end; tok = tok->next()) {
        if (Token::Match(tok, "sizeof|typeof (")) {
            tok = tok->next()->link();
            continue;
        }

        if (Token::Match(tok, "asm ( %str% ) ;"))
            return true;

        if (tok->varId() != var.declarationId())
            continue;

        if (!membervar.empty()) {
            if (isMemberVariableAssignment(tok, membervar)) {
                bool assign = true;
                bool rhs = false;
                // Used for tracking if an ")" is inner or outer
                const Token *rpar = nullptr;
                for (const Token *tok2 = tok->next(); tok2; tok2 = tok2->next()) {
                    if (tok2->str() == "=")
                        rhs = true;

                    // Look at inner expressions but not outer expressions
                    if (!rpar && tok2->str() == "(")
                        rpar = tok2->link();
                    else if (tok2->str() == ")") {
                        // No rpar => this is an outer right parenthesis
                        if (!rpar)
                            break;
                        if (rpar == tok2)
                            rpar = nullptr;
                    }

                    if (tok2->str() == ";" || (!rpar && tok2->str() == ","))
                        break;
                    if (rhs && tok2->varId() == var.declarationId() && isMemberVariableUsage(tok2, var.isPointer(), alloc, membervar)) {
                        assign = false;
                        break;
                    }
                }
                if (assign)
                    return true;
            }

            if (isMemberVariableUsage(tok, var.isPointer(), alloc, membervar))
                usetok = tok;
            else if (Token::Match(tok->previous(), "[(,] %name% [,)]"))
                return true;
        } else {
            if (isVariableUsage(tok, var.isPointer(), alloc))
                usetok = tok;
            else if (tok->strAt(1) == "=") {
                // Is var used in rhs?
                bool rhs = false;
                std::stack<const Token *> tokens;
                tokens.push(tok->next()->astOperand2());
                while (!tokens.empty()) {
                    const Token *t = tokens.top();
                    tokens.pop();
                    if (!t)
                        continue;
                    if (t->varId() == var.declarationId()) {
                        // var is used in rhs
                        rhs = true;
                        break;
                    }
                    if (Token::simpleMatch(t->previous(),"sizeof ("))
                        continue;
                    tokens.push(t->astOperand1());
                    tokens.push(t->astOperand2());
                }
                if (!rhs)
                    return true;
            } else {
                return true;
            }
        }
    }

    if (!suppressErrors && usetok) {
        if (membervar.empty())
            uninitvarError(usetok, usetok->str(), alloc);
        else
            uninitStructMemberError(usetok, usetok->str() + "." + membervar);
        return true;
    }

    return false;
}

void CheckUninitVar::checkRhs(const Token *tok, const Variable &var, Alloc alloc, unsigned int number_of_if, const std::string &membervar)
{
    bool rhs = false;
    unsigned int indent = 0;
    while (nullptr != (tok = tok->next())) {
        if (tok->str() == "=")
            rhs = true;
        else if (rhs && tok->varId() == var.declarationId()) {
            if (membervar.empty() && isVariableUsage(tok, var.isPointer(), alloc))
                uninitvarError(tok, tok->str(), alloc);
            else if (!membervar.empty() && isMemberVariableUsage(tok, var.isPointer(), alloc, membervar))
                uninitStructMemberError(tok, tok->str() + "." + membervar);
            else if (Token::Match(tok, "%var% ="))
                break;
        } else if (tok->str() == ";" || (indent==0 && tok->str() == ","))
            break;
        else if (tok->str() == "(")
            ++indent;
        else if (tok->str() == ")") {
            if (indent == 0)
                break;
            --indent;
        } else if (tok->str() == "?" && tok->astOperand2()) {
            const bool used1 = isVariableUsed(tok->astOperand2()->astOperand1(), var);
            const bool used0 = isVariableUsed(tok->astOperand2()->astOperand2(), var);
            const bool err = (number_of_if == 0) ? (used1 || used0) : (used1 && used0);
            if (err)
                uninitvarError(tok, var.nameToken()->str(), alloc);
            break;
        } else if (Token::simpleMatch(tok, "sizeof ("))
            tok = tok->next()->link();
    }
}

bool CheckUninitVar::isVariableUsage(const Token *vartok, bool pointer, Alloc alloc) const
{
    if (alloc == NO_ALLOC && ((Token::Match(vartok->previous(), "return|delete %var% !!=")) || (vartok->strAt(-1) == "]" && vartok->linkAt(-1)->strAt(-1) == "delete")))
        return true;

    // Passing variable to typeof/__alignof__
    if (Token::Match(vartok->tokAt(-3), "typeof|__alignof__ ( * %name%"))
        return false;

    // Accessing Rvalue member using "." or "->"
    if (Token::Match(vartok->previous(), "!!& %var% .")) {
        // Is struct member passed to function?
        if (!pointer && Token::Match(vartok->previous(), "[,(] %name% . %name%")) {
            // TODO: there are FN currently:
            // - should only return false if struct member is (or might be) array.
            // - should only return false if function argument is (or might be) non-const pointer or reference
            const Token *tok2 = vartok->next();
            do {
                tok2 = tok2->tokAt(2);
            } while (Token::Match(tok2, ". %name%"));
            if (Token::Match(tok2, "[,)]"))
                return false;
        } else if (pointer && alloc != CTOR_CALL && Token::Match(vartok, "%name% . %name% (")) {
            return true;
        }

        bool assignment = false;
        const Token* parent = vartok->astParent();
        while (parent) {
            if (parent->str() == "=") {
                assignment = true;
                break;
            }
            if (alloc != NO_ALLOC && parent->str() == "(") {
                if (!mSettings->library.isFunctionConst(parent->strAt(-1), true)) {
                    assignment = true;
                    break;
                }
            }
            parent = parent->astParent();
        }
        if (!assignment)
            return true;
    }

    // Passing variable to function..
    if (Token::Match(vartok->previous(), "[(,] %name% [,)]") || Token::Match(vartok->tokAt(-2), "[(,] & %name% [,)]")) {
        const int use = isFunctionParUsage(vartok, pointer, alloc);
        if (use >= 0)
            return (use>0);
    }

    if (Token::Match(vartok->previous(), "++|--|%cop%")) {
        if (mTokenizer->isCPP() && alloc == ARRAY && Token::Match(vartok->tokAt(-4), "& %var% =|( *"))
            return false;

        if (isLikelyStreamRead(mTokenizer->isCPP(), vartok->previous()))
            return false;

        if (mTokenizer->isCPP() && Token::simpleMatch(vartok->previous(), "<<")) {
            const Token* tok2 = vartok->previous();

            // Looks like stream operator, but could also initialize the variable. Check lhs.
            do {
                tok2 = tok2->astOperand1();
            } while (Token::simpleMatch(tok2, "<<"));
            if (tok2 && tok2->strAt(-1) == "::")
                tok2 = tok2->previous();
            if (tok2 && (Token::simpleMatch(tok2->previous(), "std ::") || (tok2->variable() && tok2->variable()->isStlType()) || tok2->isStandardType() || tok2->isEnumType()))
                return true;

            const Variable *var = vartok->tokAt(-2)->variable();
            return (var && (var->typeStartToken()->isStandardType() || var->typeStartToken()->isEnumType()));
        }

        // is there something like: ; "*((&var ..expr.. ="  => the variable is assigned
        if (vartok->previous()->str() == "&" && !vartok->previous()->astOperand2())
            return false;

        // bailout to avoid fp for 'int x = 2 + x();' where 'x()' is a unseen preprocessor macro (seen in linux)
        if (!pointer && vartok->next() && vartok->next()->str() == "(")
            return false;

        if (vartok->previous()->str() != "&" || !Token::Match(vartok->tokAt(-2), "[(,=?:]")) {
            if (alloc != NO_ALLOC && vartok->previous()->str() == "*") {
                // TestUninitVar::isVariableUsageDeref()
                const Token *parent = vartok->previous()->astParent();
                if (parent && parent->str() == "=" && parent->astOperand1() == vartok->previous())
                    return false;
                if (vartok->variable() && vartok->variable()->dimensions().size() >= 2)
                    return false;
                return true;
            }
            return alloc == NO_ALLOC;
        }
    }

    if (alloc == NO_ALLOC && Token::Match(vartok->previous(), "= %name% ;|%cop%")) {
        // taking reference?
        const Token *prev = vartok->tokAt(-2);
        while (Token::Match(prev, "%name%|*"))
            prev = prev->previous();
        if (!Token::simpleMatch(prev, "&"))
            return true;
    }

    bool unknown = false;
    if (pointer && alloc == NO_ALLOC && CheckNullPointer::isPointerDeRef(vartok, unknown)) {
        // function parameter?
        bool functionParameter = false;
        if (Token::Match(vartok->tokAt(-2), "%name% (") || vartok->previous()->str() == ",")
            functionParameter = true;

        // if this is not a function parameter report this dereference as variable usage
        if (!functionParameter)
            return true;
    } else if (alloc != NO_ALLOC && Token::Match(vartok, "%var% [")) {
        const Token *parent = vartok->next()->astParent();
        while (Token::Match(parent, "[|."))
            parent = parent->astParent();
        if (Token::simpleMatch(parent, "&") && !parent->astOperand2())
            return false;
        if (parent && Token::Match(parent->previous(), "if|while|switch ("))
            return true;
        if (Token::Match(parent, "[=,(]"))
            return false;
        return true;
    }

    if (mTokenizer->isCPP() && Token::simpleMatch(vartok->next(), "<<")) {
        // Is this calculation done in rhs?
        const Token *tok = vartok;
        while (Token::Match(tok, "%name%|.|::"))
            tok = tok->previous();
        if (Token::Match(tok, "[;{}]"))
            return false;

        // Is variable a known POD type then this is a variable usage,
        // otherwise we assume it's not.
        return (vartok->valueType() && vartok->valueType()->isIntegral());
    }

    if (alloc == NO_ALLOC && vartok->next() && vartok->next()->isOp() && !vartok->next()->isAssignmentOp())
        return true;

    if (vartok->strAt(1) == "]")
        return true;

    return false;
}

/***
 * Is function parameter "used" so a "usage of uninitialized variable" can
 * be written? If parameter is passed "by value" then it is "used". If it
 * is passed "by reference" then it is not necessarily "used".
 * @return  -1 => unknown   0 => not used   1 => used
 */
int CheckUninitVar::isFunctionParUsage(const Token *vartok, bool pointer, Alloc alloc) const
{
    if (!Token::Match(vartok->previous(), "[(,]") && !Token::Match(vartok->tokAt(-2), "[(,] &"))
        return -1;

    // locate start parentheses in function call..
    unsigned int argumentNumber = 0;
    const Token *start = vartok;
    while (start && !Token::Match(start, "[;{}(]")) {
        if (start->str() == ")")
            start = start->link();
        else if (start->str() == ",")
            ++argumentNumber;
        start = start->previous();
    }
    if (!start)
        return -1;

    if (Token::simpleMatch(start->link(), ") {") && Token::Match(start->previous(), "if|for|while|switch"))
        return (!pointer || alloc == NO_ALLOC);

    // is this a function call?
    if (Token::Match(start->previous(), "%name% (")) {
        const bool address(vartok->previous()->str() == "&");
        const bool array(vartok->variable() && vartok->variable()->isArray());
        // check how function handle uninitialized data arguments..
        const Function *func = start->previous()->function();
        if (func) {
            const Variable *arg = func->getArgumentVar(argumentNumber);
            if (arg) {
                const Token *argStart = arg->typeStartToken();
                if (!address && !array && Token::Match(argStart, "%type% %name%| [,)]"))
                    return 1;
                if (pointer && !address && alloc == NO_ALLOC && Token::Match(argStart,  "%type% * %name% [,)]"))
                    return 1;
                while (argStart->previous() && argStart->previous()->isName())
                    argStart = argStart->previous();
                if (Token::Match(argStart, "const %type% & %name% [,)]")) {
                    // If it's a record it's ok to pass a partially uninitialized struct.
                    if (vartok->variable() && vartok->variable()->valueType() && vartok->variable()->valueType()->type == ValueType::Type::RECORD)
                        return -1;
                    return 1;
                }
                if ((pointer || address) && alloc == NO_ALLOC && Token::Match(argStart, "const struct| %type% * %name% [,)]"))
                    return 1;
                if ((pointer || address) && Token::Match(argStart, "const %type% %name% [") && Token::Match(argStart->linkAt(3), "] [,)]"))
                    return 1;
            }

        } else if (Token::Match(start->previous(), "if|while|for")) {
            // control-flow statement reading the variable "by value"
            return alloc == NO_ALLOC;
        } else {
            const bool isnullbad = mSettings->library.isnullargbad(start->previous(), argumentNumber + 1);
            if (pointer && !address && isnullbad && alloc == NO_ALLOC)
                return 1;
            const bool isuninitbad = mSettings->library.isuninitargbad(start->previous(), argumentNumber + 1);
            if (alloc != NO_ALLOC)
                return isnullbad && isuninitbad;
            return isuninitbad && (!address || isnullbad);
        }
    }

    // unknown
    return -1;
}

bool CheckUninitVar::isMemberVariableAssignment(const Token *tok, const std::string &membervar) const
{
    if (Token::Match(tok, "%name% . %name%") && tok->strAt(2) == membervar) {
        if (Token::Match(tok->tokAt(3), "[=.[]"))
            return true;
        else if (Token::Match(tok->tokAt(-2), "[(,=] &"))
            return true;
        else if (isLikelyStreamRead(mTokenizer->isCPP(), tok->previous()))
            return true;
        else if ((tok->previous() && tok->previous()->isConstOp()) || Token::Match(tok->previous(), "[|="))
            ; // member variable usage
        else if (tok->tokAt(3)->isConstOp())
            ; // member variable usage
        else if (Token::Match(tok->previous(), "[(,] %name% . %name% [,)]") &&
                 1 == isFunctionParUsage(tok, false, NO_ALLOC)) {
            return false;
        } else
            return true;
    } else if (tok->strAt(1) == "=")
        return true;
    else if (Token::Match(tok, "%var% . %name% (")) {
        const Token *ftok = tok->tokAt(2);
        if (!ftok->function() || !ftok->function()->isConst())
            // TODO: Try to determine if membervar is assigned in method
            return true;
    } else if (tok->strAt(-1) == "&") {
        if (Token::Match(tok->tokAt(-2), "[(,] & %name%")) {
            // locate start parentheses in function call..
            unsigned int argumentNumber = 0;
            const Token *ftok = tok;
            while (ftok && !Token::Match(ftok, "[;{}(]")) {
                if (ftok->str() == ")")
                    ftok = ftok->link();
                else if (ftok->str() == ",")
                    ++argumentNumber;
                ftok = ftok->previous();
            }

            // is this a function call?
            ftok = ftok ? ftok->previous() : nullptr;
            if (Token::Match(ftok, "%name% (")) {
                // check how function handle uninitialized data arguments..
                const Function *function = ftok->function();
                const Variable *arg      = function ? function->getArgumentVar(argumentNumber) : nullptr;
                const Token *argStart    = arg ? arg->typeStartToken() : nullptr;
                while (argStart && argStart->previous() && argStart->previous()->isName())
                    argStart = argStart->previous();
                if (Token::Match(argStart, "const struct| %type% * const| %name% [,)]"))
                    return false;
            }

            else if (ftok && Token::simpleMatch(ftok->previous(), "= * ("))
                return false;
        }
        return true;
    }
    return false;
}

bool CheckUninitVar::isMemberVariableUsage(const Token *tok, bool isPointer, Alloc alloc, const std::string &membervar) const
{
    if (Token::Match(tok->previous(), "[(,] %name% . %name% [,)]") &&
        tok->strAt(2) == membervar) {
        const int use = isFunctionParUsage(tok, isPointer, alloc);
        if (use == 1)
            return true;
    }

    if (isMemberVariableAssignment(tok, membervar))
        return false;

    if (Token::Match(tok, "%name% . %name%") && tok->strAt(2) == membervar && !(tok->tokAt(-2)->variable() && tok->tokAt(-2)->variable()->isReference()))
        return true;
    else if (!isPointer && Token::Match(tok->previous(), "[(,] %name% [,)]") && isVariableUsage(tok, isPointer, alloc))
        return true;

    else if (!isPointer && Token::Match(tok->previous(), "= %name% ;"))
        return true;

    // = *(&var);
    else if (!isPointer &&
             Token::simpleMatch(tok->astParent(),"&") &&
             Token::simpleMatch(tok->astParent()->astParent(),"*") &&
             Token::Match(tok->astParent()->astParent()->astParent(), "= * (| &") &&
             tok->astParent()->astParent()->astParent()->astOperand2() == tok->astParent()->astParent())
        return true;

    else if (mSettings->experimental &&
             !isPointer &&
             Token::Match(tok->tokAt(-2), "[(,] & %name% [,)]") &&
             isVariableUsage(tok, isPointer, alloc))
        return true;

    return false;
}

void CheckUninitVar::uninitstringError(const Token *tok, const std::string &varname, bool strncpy_)
{
    reportError(tok, Severity::error, "uninitstring", "$symbol:" + varname + "\nDangerous usage of '$symbol'" + (strncpy_ ? " (strncpy doesn't always null-terminate it)." : " (not null-terminated)."), CWE676, false);
}

void CheckUninitVar::uninitdataError(const Token *tok, const std::string &varname)
{
    reportError(tok, Severity::error, "uninitdata", "$symbol:" + varname + "\nMemory is allocated but not initialized: $symbol", CWE908, false);
}

void CheckUninitVar::uninitvarError(const Token *tok, const std::string &varname)
{
    reportError(tok, Severity::error, "uninitvar", "$symbol:" + varname + "\nUninitialized variable: $symbol", CWE908, false);
}

void CheckUninitVar::uninitStructMemberError(const Token *tok, const std::string &membername)
{
    reportError(tok,
                Severity::error,
                "uninitStructMember",
                "$symbol:" + membername + "\nUninitialized struct member: $symbol", CWE908, false);
}

void CheckUninitVar::valueFlowUninit()
{
    const SymbolDatabase *symbolDatabase = mTokenizer->getSymbolDatabase();

    // check every executable scope
    for (const Scope &scope : symbolDatabase->scopeList) {
        if (!scope.isExecutable())
            continue;
        for (const Token* tok = scope.bodyStart; tok != scope.bodyEnd; tok = tok->next()) {
            if (Token::simpleMatch(tok, "sizeof (")) {
                tok = tok->linkAt(1);
                continue;
            }
            if (!tok->variable() || tok->values().size() != 1U)
                continue;
            const ValueFlow::Value &v = tok->values().front();
            if (v.valueType != ValueFlow::Value::UNINIT || v.isInconclusive())
                continue;
            if (!isVariableUsage(tok, tok->variable()->isPointer(), NO_ALLOC))
                continue;
            uninitvarError(tok, tok->str());
        }
    }
}

void CheckUninitVar::deadPointer()
{
    const SymbolDatabase *symbolDatabase = mTokenizer->getSymbolDatabase();

    // check every executable scope
    for (const Scope &scope : symbolDatabase->scopeList) {
        if (!scope.isExecutable())
            continue;
        // Dead pointers..
        for (const Token* tok = scope.bodyStart; tok != scope.bodyEnd; tok = tok->next()) {
            if (tok->variable() &&
                tok->variable()->isPointer() &&
                isVariableUsage(tok, true, NO_ALLOC)) {
                const Token *alias = tok->getValueTokenDeadPointer();
                if (alias) {
                    deadPointerError(tok,alias);
                }
            }
        }
    }
}

void CheckUninitVar::deadPointerError(const Token *pointer, const Token *alias)
{
    const std::string strpointer(pointer ? pointer->str() : std::string("pointer"));
    const std::string stralias(alias ? alias->expressionString() : std::string("&x"));

    reportError(pointer,
                Severity::error,
                "deadpointer",
                "$symbol:" + strpointer + "\nDead pointer usage. Pointer '$symbol' is dead if it has been assigned '" + stralias + "' at line " + MathLib::toString(alias ? alias->linenr() : 0U) + ".", CWE825, false);
}

static void writeFunctionArgsXml(const std::list<CheckUninitVar::MyFileInfo::FunctionArg> &list, const std::string &elementName, std::ostream &out)
{
    for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it = list.begin(); it != list.end(); ++it)
        out << "    <" << elementName
            << " id=\"" << it->id << '\"'
            << " functionName=\"" << it->functionName << '\"'
            << " argnr=\"" << it->argnr << '\"'
            << " variableName=\"" << it->variableName << "\""
            << " fileName=\"" << it->location.fileName << '\"'
            << " linenr=\"" << it->location.linenr << '\"'
            << "/>\n";
}

std::string CheckUninitVar::MyFileInfo::toString() const
{
    std::ostringstream ret;
    writeFunctionArgsXml(uninitialized, "uninitialized", ret);
    writeFunctionArgsXml(readData, "readData", ret);
    writeFunctionArgsXml(nullPointer, "nullPointer", ret);
    writeFunctionArgsXml(dereferenced, "dereferenced", ret);
    writeFunctionArgsXml(nestedCall, "nestedCall", ret);
    return ret.str();
}

#define FUNCTION_ID(function)  mTokenizer->list.file(function->tokenDef) + ':' + MathLib::toString(function->tokenDef->linenr())

CheckUninitVar::MyFileInfo::FunctionArg::FunctionArg(const Tokenizer *mTokenizer, const Scope *scope, unsigned int argnr_, const Token *tok)
    :
    id(FUNCTION_ID(scope->function)),
    functionName(scope->className),
    argnr(argnr_),
    argnr2(0),
    variableName(scope->function->getArgumentVar(argnr-1)->name())
{
    location.fileName = mTokenizer->list.file(tok);
    location.linenr   = tok->linenr();
}

bool CheckUninitVar::isUnsafeFunction(const Scope *scope, int argnr, const Token **tok) const
{
    const Variable * const argvar = scope->function->getArgumentVar(argnr);
    if (!argvar->isPointer())
        return false;
    for (const Token *tok2 = scope->bodyStart; tok2 != scope->bodyEnd; tok2 = tok2->next()) {
        if (Token::simpleMatch(tok2, ") {")) {
            tok2 = tok2->linkAt(1);
            if (Token::findmatch(tok2->link(), "return|throw", tok2))
                return false;
            if (isVariableChanged(tok2->link(), tok2, argvar->declarationId(), false, mSettings, mTokenizer->isCPP()))
                return false;
        }
        if (tok2->variable() != argvar)
            continue;
        if (!isVariableUsage(tok2, true, Alloc::ARRAY))
            return false;
        *tok = tok2;
        return true;
    }
    return false;
}

static int isCallFunction(const Scope *scope, int argnr, const Token **tok)
{
    const Variable * const argvar = scope->function->getArgumentVar(argnr);
    if (!argvar->isPointer())
        return -1;
    for (const Token *tok2 = scope->bodyStart; tok2 != scope->bodyEnd; tok2 = tok2->next()) {
        if (tok2->variable() != argvar)
            continue;
        if (!Token::Match(tok2->previous(), "[(,] %var% [,)]"))
            break;
        int argnr2 = 1;
        const Token *prev = tok2;
        while (prev && prev->str() != "(") {
            if (Token::Match(prev,"]|)"))
                prev = prev->link();
            else if (prev->str() == ",")
                ++argnr2;
            prev = prev->previous();
        }
        if (!prev || !Token::Match(prev->previous(), "%name% ("))
            break;
        if (!prev->astOperand1() || !prev->astOperand1()->function())
            break;
        *tok = prev->previous();
        return argnr2;
    }
    return -1;
}

Check::FileInfo *CheckUninitVar::getFileInfo(const Tokenizer *tokenizer, const Settings *settings) const
{
    const CheckUninitVar checker(tokenizer, settings, nullptr);
    return checker.getFileInfo();
}

Check::FileInfo *CheckUninitVar::getFileInfo() const
{
    const SymbolDatabase * const symbolDatabase = mTokenizer->getSymbolDatabase();
    std::list<Scope>::const_iterator scope;

    MyFileInfo *fileInfo = new MyFileInfo;

    // Parse all functions in TU
    for (scope = symbolDatabase->scopeList.begin(); scope != symbolDatabase->scopeList.end(); ++scope) {
        if (!scope->isExecutable() || scope->type != Scope::eFunction || !scope->function)
            continue;
        const Function *const function = scope->function;

        // function calls where uninitialized data is passed by address
        for (const Token *tok = scope->bodyStart; tok != scope->bodyEnd; tok = tok->next()) {
            if (tok->str() != "(" || !tok->astOperand1() || !tok->astOperand2())
                continue;
            if (!tok->astOperand1()->function())
                continue;
            const std::vector<const Token *> args(getArguments(tok->previous()));
            for (int argnr = 0; argnr < args.size(); ++argnr) {
                const Token *argtok = args[argnr];
                if (!argtok)
                    continue;
                if (argtok->valueType() && argtok->valueType()->pointer > 0) {
                    // null pointer..
                    const ValueFlow::Value *value = argtok->getValue(0);
                    if (value && !value->isInconclusive())
                        fileInfo->nullPointer.push_back(MyFileInfo::FunctionArg(FUNCTION_ID(tok->astOperand1()->function()), tok->astOperand1()->str(), argnr+1, mTokenizer->list.file(argtok), argtok->linenr(), argtok->str()));
                }
                // pointer to uninitialized data..
                if (argtok->str() != "&" || argtok->astOperand2())
                    continue;
                argtok = argtok->astOperand1();
                if (!argtok || !argtok->valueType() || argtok->valueType()->pointer != 0)
                    continue;
                if (argtok->values().size() != 1U)
                    continue;
                const ValueFlow::Value &v = argtok->values().front();
                if (v.valueType != ValueFlow::Value::UNINIT || v.isInconclusive())
                    continue;
                fileInfo->uninitialized.push_back(MyFileInfo::FunctionArg(FUNCTION_ID(tok->astOperand1()->function()), tok->astOperand1()->str(), argnr+1, mTokenizer->list.file(argtok), argtok->linenr(), argtok->str()));
            }
        }

        // "Unsafe" functions unconditionally reads data before it is written..
        CheckNullPointer checkNullPointer(mTokenizer, mSettings, mErrorLogger);
        for (int argnr = 0; argnr < function->argCount(); ++argnr) {
            const Token *tok;
            if (isUnsafeFunction(&*scope, argnr, &tok))
                fileInfo->readData.push_back(MyFileInfo::FunctionArg(mTokenizer, &*scope, argnr+1, tok));
            if (checkNullPointer.isUnsafeFunction(&*scope, argnr, &tok))
                fileInfo->dereferenced.push_back(MyFileInfo::FunctionArg(mTokenizer, &*scope, argnr+1, tok));
        }

        // Nested function calls
        for (int argnr = 0; argnr < function->argCount(); ++argnr) {
            const Token *tok;
            int argnr2 = isCallFunction(&*scope, argnr, &tok);
            if (argnr2 > 0) {
                MyFileInfo::FunctionArg fa(mTokenizer, &*scope, argnr+1, tok);
                fa.id  = FUNCTION_ID(function);
                fa.id2 = FUNCTION_ID(tok->function());
                fa.argnr2 = argnr2;
                fileInfo->nestedCall.push_back(fa);
            }
        }
    }

    return fileInfo;
}

Check::FileInfo * CheckUninitVar::loadFileInfoFromXml(const tinyxml2::XMLElement *xmlElement) const
{
    MyFileInfo *fileInfo = nullptr;
    for (const tinyxml2::XMLElement *e = xmlElement->FirstChildElement(); e; e = e->NextSiblingElement()) {
        const char *id = e->Attribute("id");
        if (!id)
            continue;
        const char *functionName = e->Attribute("functionName");
        if (!functionName)
            continue;
        const char *argnr = e->Attribute("argnr");
        if (!argnr || !MathLib::isInt(argnr))
            continue;
        const char *fileName = e->Attribute("fileName");
        if (!fileName)
            continue;
        const char *linenr = e->Attribute("linenr");
        if (!linenr || !MathLib::isInt(linenr))
            continue;
        const char *variableName = e->Attribute("variableName");
        if (!variableName)
            continue;
        const MyFileInfo::FunctionArg fa(id, functionName, MathLib::toLongNumber(argnr), fileName, MathLib::toLongNumber(linenr), variableName);
        if (!fileInfo)
            fileInfo = new MyFileInfo;
        if (std::strcmp(e->Name(), "uninitialized") == 0)
            fileInfo->uninitialized.push_back(fa);
        else if (std::strcmp(e->Name(), "readData") == 0)
            fileInfo->readData.push_back(fa);
        else if (std::strcmp(e->Name(), "nullPointer") == 0)
            fileInfo->nullPointer.push_back(fa);
        else if (std::strcmp(e->Name(), "dereferenced") == 0)
            fileInfo->dereferenced.push_back(fa);
        else if (std::strcmp(e->Name(), "nestedCall") == 0)
            fileInfo->nestedCall.push_back(fa);
        else
            throw InternalError(nullptr, "Wrong analyze info");
    }
    return fileInfo;
}

static bool findPath(const CheckUninitVar::MyFileInfo::FunctionArg &from,
                     const CheckUninitVar::MyFileInfo::FunctionArg &to,
                     const std::map<std::string, std::list<CheckUninitVar::MyFileInfo::FunctionArg>> &nestedCalls)
{
    if (from.id == to.id && from.argnr == to.argnr)
        return true;

    const std::map<std::string, std::list<CheckUninitVar::MyFileInfo::FunctionArg>>::const_iterator nc = nestedCalls.find(from.id);
    if (nc == nestedCalls.end())
        return false;

    for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it = nc->second.begin(); it != nc->second.end(); ++it) {
        if (from.id == it->id && from.argnr == it->argnr && it->id2 == to.id && it->argnr2 == to.argnr)
            return true;
    }

    return false;
}

bool CheckUninitVar::analyseWholeProgram(const std::list<Check::FileInfo*> &fileInfo, const Settings& settings, ErrorLogger &errorLogger)
{
    (void)settings; // This argument is unused

    // Merge all fileinfo..
    MyFileInfo all;
    for (std::list<Check::FileInfo *>::const_iterator it = fileInfo.begin(); it != fileInfo.end(); ++it) {
        const MyFileInfo *fi = dynamic_cast<MyFileInfo*>(*it);
        if (!fi)
            continue;
        all.uninitialized.insert(all.uninitialized.end(), fi->uninitialized.begin(), fi->uninitialized.end());
        all.readData.insert(all.readData.end(), fi->readData.begin(), fi->readData.end());
        all.nullPointer.insert(all.nullPointer.end(), fi->nullPointer.begin(), fi->nullPointer.end());
        all.dereferenced.insert(all.dereferenced.end(), fi->dereferenced.begin(), fi->dereferenced.end());
        all.nestedCall.insert(all.nestedCall.end(), fi->nestedCall.begin(), fi->nestedCall.end());
    }

    bool foundErrors = false;

    std::map<std::string, std::list<CheckUninitVar::MyFileInfo::FunctionArg>> nestedCalls;
    for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it = all.nestedCall.begin(); it != all.nestedCall.end(); ++it) {
        std::list<CheckUninitVar::MyFileInfo::FunctionArg> &list = nestedCalls[it->id];
        list.push_back(*it);
    }

    for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it1 = all.uninitialized.begin(); it1 != all.uninitialized.end(); ++it1) {
        const CheckUninitVar::MyFileInfo::FunctionArg &uninitialized = *it1;
        for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it2 = all.readData.begin(); it2 != all.readData.end(); ++it2) {
            const CheckUninitVar::MyFileInfo::FunctionArg &readData = *it2;

            if (!findPath(*it1, *it2, nestedCalls))
                continue;

            ErrorLogger::ErrorMessage::FileLocation fileLoc1;
            fileLoc1.setfile(uninitialized.location.fileName);
            fileLoc1.line = uninitialized.location.linenr;
            fileLoc1.setinfo("Calling function " + uninitialized.functionName + ", variable " + uninitialized.variableName + " is uninitialized");

            ErrorLogger::ErrorMessage::FileLocation fileLoc2;
            fileLoc2.setfile(readData.location.fileName);
            fileLoc2.line = readData.location.linenr;
            fileLoc2.setinfo("Using argument " + readData.variableName);

            std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
            locationList.push_back(fileLoc1);
            locationList.push_back(fileLoc2);

            const ErrorLogger::ErrorMessage errmsg(locationList,
                                                   emptyString,
                                                   Severity::error,
                                                   "using argument " + readData.variableName + " that points at uninitialized variable " + uninitialized.variableName,
                                                   "ctuuninitvar",
                                                   CWE908, false);
            errorLogger.reportErr(errmsg);

            foundErrors = true;
        }
    }

    // Null pointer checking
    // TODO: This does not belong here.
    for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it1 = all.nullPointer.begin(); it1 != all.nullPointer.end(); ++it1) {
        const CheckUninitVar::MyFileInfo::FunctionArg &nullPointer = *it1;
        for (std::list<CheckUninitVar::MyFileInfo::FunctionArg>::const_iterator it2 = all.dereferenced.begin(); it2 != all.dereferenced.end(); ++it2) {
            const CheckUninitVar::MyFileInfo::FunctionArg &dereference = *it2;

            if (!findPath(*it1, *it2, nestedCalls))
                continue;

            ErrorLogger::ErrorMessage::FileLocation fileLoc1;
            fileLoc1.setfile(nullPointer.location.fileName);
            fileLoc1.line = nullPointer.location.linenr;
            fileLoc1.setinfo("Calling function " + nullPointer.functionName + ", " + MathLib::toString(nullPointer.argnr) + getOrdinalText(nullPointer.argnr) + " argument is null");

            ErrorLogger::ErrorMessage::FileLocation fileLoc2;
            fileLoc2.setfile(dereference.location.fileName);
            fileLoc2.line = dereference.location.linenr;
            fileLoc2.setinfo("Dereferencing argument " + dereference.variableName + " that is null");

            std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
            locationList.push_back(fileLoc1);
            locationList.push_back(fileLoc2);

            const ErrorLogger::ErrorMessage errmsg(locationList,
                                                   emptyString,
                                                   Severity::error,
                                                   "Null pointer dereference: " + dereference.variableName,
                                                   "ctunullpointer",
                                                   CWE476, false);
            errorLogger.reportErr(errmsg);

            foundErrors = true;
        }
    }

    return foundErrors;
}
