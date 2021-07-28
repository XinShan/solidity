/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * @author Christian <c@ethdev.com>
 * @date 2017
 * Converts a parsed assembly into its textual form.
 */

#include <libyul/AsmPrinter.h>
#include <libyul/AST.h>
#include <libyul/Exceptions.h>
#include <libyul/Dialect.h>

#include <libsolutil/CommonData.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <range/v3/view/transform.hpp>

#include <memory>
#include <functional>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::yul;

string AsmPrinter::operator()(Literal const& _literal)
{
	string const locationComment = formatSourceLocationComment(_literal.debugData, !m_insideExpression);

	switch (_literal.kind)
	{
	case LiteralKind::Number:
		yulAssert(isValidDecimal(_literal.value.str()) || isValidHex(_literal.value.str()), "Invalid number literal");
		return locationComment + _literal.value.str() + appendTypeName(_literal.type);
	case LiteralKind::Boolean:
		yulAssert(_literal.value == "true"_yulstring || _literal.value == "false"_yulstring, "Invalid bool literal.");
		return locationComment + ((_literal.value == "true"_yulstring) ? "true" : "false") + appendTypeName(_literal.type, true);
	case LiteralKind::String:
		break;
	}

	return locationComment + escapeAndQuoteString(_literal.value.str()) + appendTypeName(_literal.type);
}

string AsmPrinter::operator()(Identifier const& _identifier)
{
	yulAssert(!_identifier.name.empty(), "Invalid identifier.");
	return formatSourceLocationComment(_identifier.debugData, !m_insideExpression) + _identifier.name.str();
}

string AsmPrinter::operator()(ExpressionStatement const& _statement)
{
	string const locationComment = formatSourceLocationComment(_statement.debugData, true);
	m_insideExpression++;
	ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });

	return locationComment + std::visit(*this, _statement.expression);
}

string AsmPrinter::operator()(Assignment const& _assignment)
{
	yulAssert(_assignment.variableNames.size() >= 1, "");
	string variables = (*this)(_assignment.variableNames.front());
	for (size_t i = 1; i < _assignment.variableNames.size(); ++i)
		variables += ", " + (*this)(_assignment.variableNames[i]);

	string const locationComment = formatSourceLocationComment(_assignment.debugData, true);
	m_insideExpression++;
	ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });

	return locationComment + variables + " := " + std::visit(*this, *_assignment.value);
}

string AsmPrinter::operator()(VariableDeclaration const& _variableDeclaration)
{
	string out = formatSourceLocationComment(_variableDeclaration.debugData, true) + "let ";
	m_insideExpression++;
	ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });

	out += boost::algorithm::join(
		_variableDeclaration.variables | ranges::views::transform(
			[this](TypedName argument) { return formatTypedName(argument); }
		),
		", "
	);
	if (_variableDeclaration.value)
	{
		out += " := ";
		out += std::visit(*this, *_variableDeclaration.value);
	}
	return out;
}

string AsmPrinter::operator()(FunctionDefinition const& _functionDefinition)
{
	yulAssert(!_functionDefinition.name.empty(), "Invalid function name.");

	string out =
		formatSourceLocationComment(_functionDefinition.debugData, true) +
		"function " +
		_functionDefinition.name.str() +
		"(";

	{
		m_insideExpression++;
		ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });

		out += boost::algorithm::join(
				_functionDefinition.parameters | ranges::views::transform(
					[this](TypedName argument) { return formatTypedName(argument); }
					),
				", "
				);
		out += ")";
		if (!_functionDefinition.returnVariables.empty())
		{
			out += " -> ";
			out += boost::algorithm::join(
					_functionDefinition.returnVariables | ranges::views::transform(
						[this](TypedName argument) { return formatTypedName(argument); }
						),
					", "
					);
		}
	}

	return  out + "\n" + (*this)(_functionDefinition.body);
}

string AsmPrinter::operator()(FunctionCall const& _functionCall)
{
	string const locationComment = formatSourceLocationComment(_functionCall.debugData, !m_insideExpression);
	string const functionName = (*this)(_functionCall.functionName);
	return
		locationComment +
		functionName + "(" +
		boost::algorithm::join(
			_functionCall.arguments | ranges::views::transform([&](auto&& _node) { return std::visit(*this, _node); }),
			", " ) +
		")";
}

string AsmPrinter::operator()(If const& _if)
{
	yulAssert(_if.condition, "Invalid if condition.");

	string const locationComment = formatSourceLocationComment(_if.debugData, true);

	string body = (*this)(_if.body);
	char delim = '\n';
	if (body.find('\n') == string::npos)
		delim = ' ';

	string out;
	{
		m_insideExpression++;
		ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });
		out = locationComment + "if " + std::visit(*this, *_if.condition);
	}

	return out + delim + (*this)(_if.body);
}

string AsmPrinter::operator()(Switch const& _switch)
{
	yulAssert(_switch.expression, "Invalid expression pointer.");

	string out;
	{
		m_insideExpression++;
		ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });
		out = formatSourceLocationComment(_switch.debugData, true);
		out += "switch " + std::visit(*this, *_switch.expression);
	}

	for (auto const& _case: _switch.cases)
	{
		if (!_case.value)
			out += "\ndefault ";
		else
		{
			m_insideExpression++;
			ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });
			out += "\ncase " + (*this)(*_case.value) + " ";
		}
		out += (*this)(_case.body);
	}
	return out;
}

string AsmPrinter::operator()(ForLoop const& _forLoop)
{
	yulAssert(_forLoop.condition, "Invalid for loop condition.");
	string const locationComment = formatSourceLocationComment(_forLoop.debugData, true);

	string pre, condition, post;
	{
		m_insideExpression++;
		ScopeGuard unsetInsideExpression([&]() { m_insideExpression--; });

		pre = (*this)(_forLoop.pre);
		condition = std::visit(*this, *_forLoop.condition);
		post = (*this)(_forLoop.post);
	}

	char delim = '\n';
	if (
		pre.size() + condition.size() + post.size() < 60 &&
		pre.find('\n') == string::npos &&
		post.find('\n') == string::npos
	)
		delim = ' ';
	return
		locationComment +
		("for " + move(pre) + delim + move(condition) + delim + move(post) + "\n") +
		(*this)(_forLoop.body);
}

string AsmPrinter::operator()(Break const& _break)
{
	return formatSourceLocationComment(_break.debugData, true) + "break";
}

string AsmPrinter::operator()(Continue const& _continue)
{
	return formatSourceLocationComment(_continue.debugData, true) + "continue";
}

string AsmPrinter::operator()(Leave const& _leave)
{
	return formatSourceLocationComment(_leave.debugData, true) + "leave";
}

string AsmPrinter::operator()(Block const& _block)
{
	string const locationComment = formatSourceLocationComment(_block.debugData, true);

	size_t originalInsideExpression = m_insideExpression;
	ScopeGuard assertInsideExpression([&]() { yulAssert(m_insideExpression == originalInsideExpression, ""); });

	if (_block.statements.empty())
		return locationComment + "{ }";
	string body = boost::algorithm::join(
		_block.statements | ranges::views::transform([&](auto&& _node) { return std::visit(*this, _node); }),
		"\n"
	);
	if (body.size() < 30 && body.find('\n') == string::npos)
		return locationComment + "{ " + body + " }";
	else
	{
		boost::replace_all(body, "\n", "\n    ");
		return locationComment + "{\n    " + body + "\n}";
	}
}

string AsmPrinter::formatTypedName(TypedName _variable)
{
	yulAssert(!_variable.name.empty(), "Invalid variable name.");
	return formatSourceLocationComment(_variable.debugData, false) + _variable.name.str() + appendTypeName(_variable.type);
}

string AsmPrinter::appendTypeName(YulString _type, bool _isBoolLiteral) const
{
	if (m_dialect && !_type.empty())
	{
		if (!_isBoolLiteral && _type == m_dialect->defaultType)
			_type = {};
		else if (_isBoolLiteral && _type == m_dialect->boolType && !m_dialect->defaultType.empty())
			// Special case: If we have a bool type but empty default type, do not remove the type.
			_type = {};
	}
	if (_type.empty())
		return {};
	else
		return ":" + _type.str();
}

std::string AsmPrinter::formatSourceLocationComment(shared_ptr<DebugData const> _debugData, bool _statement)
{
	if (!_debugData || m_lastLocation == _debugData->location || m_nameToSourceIndex.empty())
		return "";

	auto resultIter = m_nameToSourceIndex.find(*_debugData->location.sourceName);
	if (resultIter == m_nameToSourceIndex.end())
		return "";

	m_lastLocation = _debugData->location;

	return
		(_statement ? "/// @src " : "/** @src ") +
		to_string(resultIter->second) +
		":" +
		to_string(_debugData->location.start) +
		":" +
		to_string(_debugData->location.end) +
		(_statement ? "\n" : " */ ");
}
