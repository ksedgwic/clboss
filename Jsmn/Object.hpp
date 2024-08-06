#ifndef JSMN_OBJECT_HPP
#define JSMN_OBJECT_HPP

#include"Jsmn/Detail/Iterator.hpp"
#include<cstddef>
#include<istream>
#include<memory>
#include<ostream>
#include<stdexcept>
#include"Util/BacktraceException.hpp"
#include<string>
#include<vector>

namespace Jsmn { namespace Detail { struct ParseResult; }}
namespace Jsmn { class ParserExposedBuffer; }

namespace Jsmn {

/* Thrown when converting or using as incorrect type.  */
class TypeError : public Util::BacktraceException<std::invalid_argument> {
public:
	TypeError() : Util::BacktraceException<std::invalid_argument>("Incorrect type.") { }
};

/* Represents an object that has been parsed from a JSON string.  */
class Object {
private:
	class Impl;
	std::shared_ptr<Impl> pimpl;

	/* Used by class ParserExposedBuffer to construct.  */
	Object( std::shared_ptr<Detail::ParseResult>
	      , unsigned int
	      );

public:
	/* Results in a null object.  */
	Object();

	Object(Object const&) =default;
	Object(Object&&) =default;
	Object& operator=(Object const&) =default;
	Object& operator=(Object&&) =default;

	static Object parse_json(char const* txt);

	/* Type queries on the object.  */
	bool is_null() const;
	bool is_boolean() const;
	bool is_string() const;
	bool is_object() const;
	bool is_array() const;
	bool is_number() const;

	/* Conversions will fail if not of the correct type!  */
	explicit operator bool() const; /* Return false if null as well.  */
	explicit operator std::string() const;
	explicit operator double() const;

	/* Number of keys for objects, number of elements for arrays.
	 * Will throw TypeError if not object or array.
	 */
	std::size_t size() const;
	std::size_t length() const { return size(); }

	/* Act as an object.  */
	std::vector<std::string> keys() const;
	bool has(std::string const&) const;
	Object operator[](std::string const&) const; /* Return null if key not exist.  */
	/* Act as an array.  */
	Object operator[](std::size_t) const; /* Return null if out-of-range.  */
	/* TODO: Iterator.  */

	/* Formal factory.  */
	friend class ParserExposedBuffer;
	/* Iterator type.  */
	friend class Jsmn::Detail::Iterator;

	/* Access text directly, not recommended.  */
	std::string direct_text() const;
	void direct_text(char const*& t, std::size_t& len) const;

	/* Iterate over array elements.  */
	typedef Jsmn::Detail::Iterator const_iterator;
	typedef Jsmn::Detail::Iterator iterator;
	iterator begin() const;
	iterator end() const;
};

std::ostream& operator<<(std::ostream&, Jsmn::Object const&);
std::istream& operator>>(std::istream&, Jsmn::Object&);

}

#endif /* !defined(JSMN_OBJECT_HPP) */
