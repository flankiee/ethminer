/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @file ExpressionClasses.cpp
 * @author Christian <c@ethdev.com>
 * @date 2015
 * Container for equivalence classes of expressions for use in common subexpression elimination.
 */

#include <libevmcore/ExpressionClasses.h>
#include <utility>
#include <tuple>
#include <functional>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/noncopyable.hpp>
#include <libevmcore/Assembly.h>
#include <libevmcore/CommonSubexpressionEliminator.h>

using namespace std;
using namespace dev;
using namespace dev::eth;


bool ExpressionClasses::Expression::operator<(const ExpressionClasses::Expression& _other) const
{
	auto type = item->type();
	auto otherType = _other.item->type();
	return std::tie(type, item->data(), arguments) <
		std::tie(otherType, _other.item->data(), _other.arguments);
}

ExpressionClasses::Id ExpressionClasses::find(AssemblyItem const& _item, Ids const& _arguments)
{
	Expression exp;
	exp.id = Id(-1);
	exp.item = &_item;
	exp.arguments = _arguments;

	if (SemanticInformation::isCommutativeOperation(_item))
		sort(exp.arguments.begin(), exp.arguments.end());

	//@todo store all class members (not only the representatives) in an efficient data structure to search here
	for (Expression const& e: m_representatives)
		if (!(e < exp || exp < e))
			return e.id;

	if (SemanticInformation::isDupInstruction(_item))
	{
		// Special item that refers to values pre-existing on the stack
		m_spareAssemblyItem.push_back(make_shared<AssemblyItem>(_item));
		exp.item = m_spareAssemblyItem.back().get();
	}

	ExpressionClasses::Id id = tryToSimplify(exp);
	if (id < m_representatives.size())
		return id;

	exp.id = m_representatives.size();
	m_representatives.push_back(exp);
	return exp.id;
}

string ExpressionClasses::fullDAGToString(ExpressionClasses::Id _id)
{
	Expression const& expr = representative(_id);
	stringstream str;
	str << dec << expr.id << ":" << *expr.item << "(";
	for (Id arg: expr.arguments)
		str << fullDAGToString(arg) << ",";
	str << ")";
	return str.str();
}

class Rules: public boost::noncopyable
{
public:
	Rules();
	void resetMatchGroups() { m_matchGroups.clear(); }
	vector<pair<Pattern, function<Pattern()>>> rules() const { return m_rules; }

private:
	using Expression = ExpressionClasses::Expression;
	map<unsigned, Expression const*> m_matchGroups;
	vector<pair<Pattern, function<Pattern()>>> m_rules;
};

Rules::Rules()
{
	// Multiple occurences of one of these inside one rule must match the same equivalence class.
	// Constants.
	Pattern A(Push);
	Pattern B(Push);
	Pattern C(Push);
	// Anything.
	Pattern X;
	Pattern Y;
	Pattern Z;
	A.setMatchGroup(1, m_matchGroups);
	B.setMatchGroup(2, m_matchGroups);
	C.setMatchGroup(3, m_matchGroups);
	X.setMatchGroup(4, m_matchGroups);
	Y.setMatchGroup(5, m_matchGroups);
	Z.setMatchGroup(6, m_matchGroups);

	m_rules = vector<pair<Pattern, function<Pattern()>>>{
		// arithmetics on constants
		{{Instruction::ADD, {A, B}}, [=]{ return A.d() + B.d(); }},
		{{Instruction::MUL, {A, B}}, [=]{ return A.d() * B.d(); }},
		{{Instruction::SUB, {A, B}}, [=]{ return A.d() - B.d(); }},
		{{Instruction::DIV, {A, B}}, [=]{ return B.d() == 0 ? 0 : A.d() / B.d(); }},
		{{Instruction::SDIV, {A, B}}, [=]{ return B.d() == 0 ? 0 : s2u(u2s(A.d()) / u2s(B.d())); }},
		{{Instruction::MOD, {A, B}}, [=]{ return B.d() == 0 ? 0 : A.d() % B.d(); }},
		{{Instruction::SMOD, {A, B}}, [=]{ return B.d() == 0 ? 0 : s2u(u2s(A.d()) % u2s(B.d())); }},
		{{Instruction::EXP, {A, B}}, [=]{ return u256(boost::multiprecision::powm(bigint(A.d()), bigint(B.d()), bigint(1) << 256)); }},
		{{Instruction::NOT, {A}}, [=]{ return ~A.d(); }},
		{{Instruction::LT, {A, B}}, [=]() { return A.d() < B.d() ? u256(1) : 0; }},
		{{Instruction::GT, {A, B}}, [=]() -> u256 { return A.d() > B.d() ? 1 : 0; }},
		{{Instruction::SLT, {A, B}}, [=]() -> u256 { return u2s(A.d()) < u2s(B.d()) ? 1 : 0; }},
		{{Instruction::SGT, {A, B}}, [=]() -> u256 { return u2s(A.d()) > u2s(B.d()) ? 1 : 0; }},
		{{Instruction::EQ, {A, B}}, [=]() -> u256 { return A.d() == B.d() ? 1 : 0; }},
		{{Instruction::ISZERO, {A}}, [=]() -> u256 { return A.d() == 0 ? 1 : 0; }},
		{{Instruction::AND, {A, B}}, [=]{ return A.d() & B.d(); }},
		{{Instruction::OR, {A, B}}, [=]{ return A.d() | B.d(); }},
		{{Instruction::XOR, {A, B}}, [=]{ return A.d() ^ B.d(); }},
		{{Instruction::BYTE, {A, B}}, [=]{ return A.d() >= 32 ? 0 : (B.d() >> unsigned(8 * (31 - A.d()))) & 0xff; }},
		{{Instruction::ADDMOD, {A, B, C}}, [=]{ return C.d() == 0 ? 0 : u256((bigint(A.d()) + bigint(B.d())) % C.d()); }},
		{{Instruction::MULMOD, {A, B, C}}, [=]{ return C.d() == 0 ? 0 : u256((bigint(A.d()) * bigint(B.d())) % C.d()); }},
		{{Instruction::MULMOD, {A, B, C}}, [=]{ return A.d() * B.d(); }},
		{{Instruction::SIGNEXTEND, {A, B}}, [=]() -> u256 {
			if (A.d() >= 31)
				return B.d();
			unsigned testBit = unsigned(A.d()) * 8 + 7;
			u256 mask = (u256(1) << testBit) - 1;
			return u256(boost::multiprecision::bit_test(B.d(), testBit) ? B.d() | ~mask : B.d() & mask);
		}},

		// invariants involving known constants
		{{Instruction::ADD, {X, 0}}, [=]{ return X; }},
		{{Instruction::MUL, {X, 1}}, [=]{ return X; }},
		{{Instruction::DIV, {X, 1}}, [=]{ return X; }},
		{{Instruction::SDIV, {X, 1}}, [=]{ return X; }},
		{{Instruction::OR, {X, 0}}, [=]{ return X; }},
		{{Instruction::XOR, {X, 0}}, [=]{ return X; }},
		{{Instruction::AND, {X, ~u256(0)}}, [=]{ return X; }},
		{{Instruction::MUL, {X, 0}}, [=]{ return u256(0); }},
		{{Instruction::DIV, {X, 0}}, [=]{ return u256(0); }},
		{{Instruction::MOD, {X, 0}}, [=]{ return u256(0); }},
		{{Instruction::MOD, {0, X}}, [=]{ return u256(0); }},
		{{Instruction::AND, {X, 0}}, [=]{ return u256(0); }},
		{{Instruction::OR, {X, ~u256(0)}}, [=]{ return ~u256(0); }},
		// operations involving an expression and itself
		{{Instruction::AND, {X, X}}, [=]{ return X; }},
		{{Instruction::OR, {X, X}}, [=]{ return X; }},
		{{Instruction::SUB, {X, X}}, [=]{ return u256(0); }},
		{{Instruction::EQ, {X, X}}, [=]{ return u256(1); }},
		{{Instruction::EQ, {X, X}}, [=]{ return u256(1); }},
		{{Instruction::LT, {X, X}}, [=]{ return u256(0); }},
		{{Instruction::SLT, {X, X}}, [=]{ return u256(0); }},
		{{Instruction::GT, {X, X}}, [=]{ return u256(0); }},
		{{Instruction::SGT, {X, X}}, [=]{ return u256(0); }},
		{{Instruction::MOD, {X, X}}, [=]{ return u256(0); }},

		{{Instruction::NOT, {{Instruction::NOT, {X}}}}, [=]{ return X; }},
	};
	// Associative operations
	for (auto const& opFun: vector<pair<Instruction,function<u256(u256 const&,u256 const&)>>>{
		{Instruction::ADD, plus<u256>()},
		{Instruction::MUL, multiplies<u256>()},
		{Instruction::AND, bit_and<u256>()},
		{Instruction::OR, bit_or<u256>()},
		{Instruction::XOR, bit_xor<u256>()}
	})
	{
		auto op = opFun.first;
		auto fun = opFun.second;
		// Moving constants to the outside, order matters here!
		// we need actions that return expressions (or patterns?) here, and we need also reversed rules
		// (X+A)+B -> X+(A+B)
		m_rules.push_back({
			{op, {{op, {X, A}}, B}},
			[=]() -> Pattern { return {op, {X, fun(A.d(), B.d())}}; }
		});
		// X+(Y+A) -> (X+Y)+A
		m_rules.push_back({
			{op, {{op, {X, A}}, Y}},
			[=]() -> Pattern { return {op, {{op, {X, Y}}, A}}; }
		});
		// For now, we still need explicit commutativity for the inner pattern
		m_rules.push_back({
			{op, {{op, {A, X}}, B}},
			[=]() -> Pattern { return {op, {X, fun(A.d(), B.d())}}; }
		});
		m_rules.push_back({
			{op, {{op, {A, X}}, Y}},
			[=]() -> Pattern { return {op, {{op, {X, Y}}, A}}; }
		});
	};

	//@todo: (x+8)-3 and other things
}

ExpressionClasses::Id ExpressionClasses::tryToSimplify(Expression const& _expr, bool _secondRun)
{
	static Rules rules;

	if (_expr.item->type() != Operation)
		return -1;

	for (auto const& rule: rules.rules())
	{
		rules.resetMatchGroups();
		if (rule.first.matches(_expr, *this))
		{
			// Debug info
			//cout << "Simplifying " << *_expr.item << "(";
			//for (Id arg: _expr.arguments)
			//	cout << fullDAGToString(arg) << ", ";
			//cout << ")" << endl;
			//cout << "with rule " << rule.first.toString() << endl;
			//ExpressionTemplate t(rule.second());
			//cout << "to" << rule.second().toString() << endl;
			return rebuildExpression(ExpressionTemplate(rule.second()));
		}
	}

	if (!_secondRun && _expr.arguments.size() == 2 && SemanticInformation::isCommutativeOperation(*_expr.item))
	{
		Expression expr = _expr;
		swap(expr.arguments[0], expr.arguments[1]);
		return tryToSimplify(expr, true);
	}

	return -1;
}

ExpressionClasses::Id ExpressionClasses::rebuildExpression(ExpressionTemplate const& _template)
{
	if (_template.hasId)
		return _template.id;

	Ids arguments;
	for (ExpressionTemplate const& t: _template.arguments)
		arguments.push_back(rebuildExpression(t));
	m_spareAssemblyItem.push_back(make_shared<AssemblyItem>(_template.item));
	return find(*m_spareAssemblyItem.back(), arguments);
}


Pattern::Pattern(Instruction _instruction, std::vector<Pattern> const& _arguments):
	m_type(Operation),
	m_requireDataMatch(true),
	m_data(_instruction),
	m_arguments(_arguments)
{
}

void Pattern::setMatchGroup(unsigned _group, map<unsigned, Expression const*>& _matchGroups)
{
	m_matchGroup = _group;
	m_matchGroups = &_matchGroups;
}

bool Pattern::matches(Expression const& _expr, ExpressionClasses const& _classes) const
{
	if (!matchesBaseItem(*_expr.item))
		return false;
	if (m_matchGroup)
	{
		if (!m_matchGroups->count(m_matchGroup))
			(*m_matchGroups)[m_matchGroup] = &_expr;
		else if ((*m_matchGroups)[m_matchGroup]->id != _expr.id)
			return false;
	}
	assertThrow(m_arguments.size() == 0 || _expr.arguments.size() == m_arguments.size(), OptimizerException, "");
	for (size_t i = 0; i < m_arguments.size(); ++i)
		if (!m_arguments[i].matches(_classes.representative(_expr.arguments[i]), _classes))
			return false;
	return true;
}

string Pattern::toString() const
{
	stringstream s;
	switch (m_type)
	{
	case Operation:
		s << instructionInfo(Instruction(unsigned(m_data))).name;
		break;
	case Push:
		s << "PUSH " << hex << m_data;
		break;
	case UndefinedItem:
		s << "ANY";
		break;
	default:
		s << "t=" << dec << m_type << " d=" << hex << m_data;
		break;
	}
	if (!m_requireDataMatch)
		s << " ~";
	if (m_matchGroup)
		s << "[" << dec << m_matchGroup << "]";
	s << "(";
	for (Pattern const& p: m_arguments)
		s << p.toString() << ", ";
	s << ")";
	return s.str();
}

bool Pattern::matchesBaseItem(AssemblyItem const& _item) const
{
	if (m_type == UndefinedItem)
		return true;
	if (m_type != _item.type())
		return false;
	if (m_requireDataMatch && m_data != _item.data())
		return false;
	return true;
}

Pattern::Expression const& Pattern::matchGroupValue() const
{
	assertThrow(m_matchGroup > 0, OptimizerException, "");
	assertThrow(!!m_matchGroups, OptimizerException, "");
	assertThrow((*m_matchGroups)[m_matchGroup], OptimizerException, "");
	return *(*m_matchGroups)[m_matchGroup];
}


ExpressionTemplate::ExpressionTemplate(Pattern const& _pattern)
{
	if (_pattern.matchGroup())
	{
		hasId = true;
		id = _pattern.id();
	}
	else
	{
		hasId = false;
		item = _pattern.toAssemblyItem();
	}
	for (auto const& arg: _pattern.arguments())
		arguments.push_back(ExpressionTemplate(arg));
}

string ExpressionTemplate::toString() const
{
	stringstream s;
	if (hasId)
		s << id;
	else
		s << item;
	s << "(";
	for (auto const& arg: arguments)
		s << arg.toString();
	s << ")";
	return s.str();
}
