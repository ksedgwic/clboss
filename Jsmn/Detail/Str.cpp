#include<assert.h>
#include<iomanip>
#include<locale>
#include<sstream>
#include"Jsmn/Detail/Str.hpp"
#include"Util/Str.hpp"

namespace {

class StringReader {
private:
	std::string const& s;
	unsigned int i;
public:
	StringReader() =delete;
	StringReader(std::string const& s_) : s(s_), i(0) { }

	bool eof() const { return i == s.length(); }
	char pop() { return s[i++]; }
	std::string pop_str(unsigned int l) {
		auto ret = std::string(&s[i], l);
		i += l;
		return ret;
	}
};

}

namespace Jsmn { namespace Detail { namespace Str {

std::string to_escaped(std::string const& s) {
	std::ostringstream os;
	for (auto c : s) {
		switch (c) {
		case '\"': os << "\\\""; break;
		case '\\': os << "\\\\"; break;
		case '\b': os << "\\b"; break;
		case '\f': os << "\\f"; break;
		case '\n': os << "\\n"; break;
		case '\r': os << "\\r"; break;
		case '\t': os << "\\t"; break;
		default:
			if (c < 32) {
				os << "\\u00";
				os << std::hex << std::setfill('0') << std::setw(2);
				os << ((unsigned int) c);
			} else {
				os << c;
			}
			break;
		}
	}
	return os.str();
}
std::string from_escaped(std::string const& s) {
	std::ostringstream os;
	for (auto sr = StringReader(s); !sr.eof(); ) {
		auto c = sr.pop();
		if (c != '\\') {
			os << c;
			continue;
		}
		c = sr.pop();
		switch (c) {
		case '\"': os << '\"'; break;
		case '\\': os << '\\'; break;
		case 'b': os << '\b'; break;
		case 'f': os << '\f'; break;
		case 'n': os << '\n'; break;
		case 'r': os << '\r'; break;
		case 't': os << '\t'; break;
		case 'u':
			{
				auto uc = sr.pop_str(4);
				auto hex = Util::Str::hexread(uc);
				assert(hex.size() == 2);
				auto cp = (std::uint16_t(hex[0]) << 8)
					+ std::uint16_t(hex[1])
					;
				/* Re-encode in UTF-8, because UTF-8 is
				 * Sane.
				 */
				if (cp < 0x80) {
					os << ((char) cp);
				} else if (cp < 0x800) {
					/* 2-char sequence.  */
					auto c0 = char((cp >> 6) & ((1 << 5) - 1)) | 0xC0;
					auto c1 = char(cp & ((1 << 6) - 1)) | 0x80;
					os << c0 << c1;
				} else {
					/* 3-char sequence.  */
					auto c0 = char((cp >> 12) & ((1 << 4) - 1)) | 0xE0;
					auto c1 = char((cp >> 6) & ((1 << 6) - 1)) | 0x80;
					auto c2 = char(cp & ((1 << 6) - 1)) | 0x80;
					os << c0 << c1 << c2;
				}
			}
			break;
		default:
			/* Impossible case (else jsmn should have failed).  */
			assert(0 == 1);
			break;
		}
	}
	return os.str();
}

double to_double(std::string const& s) {
	/* Assumes C locale is JSON-compatible.  */
	auto is = std::istringstream(s);
	is.imbue(std::locale("C"));

	auto ret = double(0);
	is >> ret;
	return ret;
}
std::string from_double(double d) {
	/* Print with the fewest significant digits that read back as
	 * the same double.  The stream default of six digits turned a
	 * timestamp such as 1721639381.632 into 1.72164e+09.
	 * Assumes C locale is JSON-compatible.  */
	for (auto precision = 15; ; ++precision) {
		auto os = std::ostringstream();
		os.imbue(std::locale("C"));
		os << std::setprecision(precision) << d;
		if (precision >= 17)
			return os.str();
		auto is = std::istringstream(os.str());
		is.imbue(std::locale("C"));
		auto back = double();
		is >> back;
		if (back == d)
			return os.str();
	}
}

}}}
