#ifndef UTIL_STR_HPP
#define UTIL_STR_HPP

/*
 * Minor string utilities.
 */

#include"Util/BacktraceException.hpp"
#include<cstdint>
#include<stdarg.h>
#include<stdexcept>
#include<string>
#include<vector>

namespace Util {
namespace Str {

/* Outputs a two-digit hex string of the given byte.  */
std::string hexbyte(std::uint8_t);
/* Outputs a string of the given data.  */
std::string hexdump(void const* p, std::size_t s);

/* Creates a buffer from the given hex string.  */
struct HexParseFailure : public Util::BacktraceException<std::runtime_error> {
	HexParseFailure(std::string msg)
		: Util::BacktraceException<std::runtime_error>("hexread: " + msg) { }
};
std::vector<std::uint8_t> hexread(std::string const&);

/* Checks that the given string is a hex string with an
 * even number of digits.
 */
bool ishex(std::string const&);

std::string trim(std::string const& s);

/* Renders an integer with '_' between every three digit places
 * (18851040 -> "18_851_040"), for log lines carrying large msat/sat
 * amounts.  Matches the digit grouping clboss-xrebalance-view and
 * the other contrib tools print.  */
std::string group_digits(std::uint64_t);
std::string group_digits(std::int64_t);

/* Like `sprintf`.  */
std::string fmt(char const *tpl, ...)
#if HAVE_ATTRIBUTE_FORMAT
	__attribute__ ((format (printf, 1, 2)))
#endif
;
std::string vfmt(char const *tpl, va_list ap);

}}

#endif /* !defined(UTIL_STR_HPP) */
