#ifndef JSMN_PARSER_HPP
#define JSMN_PARSER_HPP

#include"Jsmn/ParserExposedBuffer.hpp"
#include<algorithm>

namespace Jsmn {

/* A stateful jsmn-based parser.
 *
 * If previously-fed data into this parser is not a complete
 * JSON datum, then the feed() member function will return an
 * empty vector.
 * Subsequent feed() calls will append their argument to any
 * incomplete datum until at least one complete datum is
 * formed.
 */
class Parser {
private:
	Jsmn::ParserExposedBuffer base;

public:
	Parser() =default;
	~Parser() =default;

	/* No expected use for these... Disable for now.  */
	Parser(Parser const&) =delete;
	Parser(Parser&&) =delete;

	/* Feeds a string into this parser.*/
	std::vector<Jsmn::Object> feed(std::string const& s) {
		base.load_buffer(s.size(), [&s](char* p) {
			std::copy(s.begin(), s.end(), p);
			return s.size();
		});
		return base.parse_buffer();
	}
};

}

#endif /* !defined(JSMN_PARSER_HPP) */
