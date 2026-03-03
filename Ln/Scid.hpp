#ifndef LN_SCID_HPP
#define LN_SCID_HPP

#include "Util/Compiler.hpp"
#include<cstddef>
#include<cstdint>
#include<string>
#include<iostream>

namespace Ln {

class Scid {
private:
	/* Most significant 3 bytes = block height.
	 * Next 3 bytes = transaction index.
	 * Lowest 2 bytes = output index.
	 */
	std::uint64_t val;

public:
	Scid(std::nullptr_t _ = nullptr) : val(0) { }
	Scid(Scid const&) =default;
	~Scid() =default;

	explicit
	operator bool() const {
		return val != 0;
	}
	bool operator!() const {
		return !bool(*this);
	}

	bool operator==(Scid const& i) const {
		return val == i.val;
	}
	bool operator!=(Scid const& i) const {
		return !(*this == i);
	}
	/* For key of maps and sets.  */
	bool operator<(Scid const& i) const {
		return val < i.val;
	}
	bool operator>(Scid const& i) const {
		return i < *this;
	}
	bool operator<=(Scid const& i) const {
		return !(*this > i);
	}
	bool operator>=(Scid const& i) const {
		return (i <= *this);
	}

	explicit
	operator std::string() const;
	explicit
	Scid(std::string const&);

	static
	bool valid_string(std::string const&);
};

inline
std::istream& operator>>(std::istream& is, Scid& chan) {
	auto s = std::string();
	is >> s;
	chan = Scid(s);
	return is;
}
inline
std::ostream& operator<<(std::ostream& os, Scid const& chan) {
	return os << std::string(chan);
}

}

#endif /* !defined(LN_SCID_HPP) */
