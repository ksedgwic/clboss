#ifndef SHA256_FUN_HPP
#define SHA256_FUN_HPP

#include<cstdint>
#include"Sha256/Hash.hpp"
#include"Sha256/Hasher.hpp"

namespace Sha256 {

inline
Sha256::Hash fun(void const* p, std::size_t len) {
	auto hasher = Sha256::Hasher();
	hasher.feed(p, len);
	return std::move(hasher).finalize();
}
/* Convenience version for double-sha256.  */
inline
Sha256::Hash fun(Sha256::Hash h) {
	std::uint8_t buf[32];
	h.to_buffer(buf);
	auto hasher = Sha256::Hasher();
	hasher.feed(buf, sizeof(buf));
	return std::move(hasher).finalize();
}

}

#endif /* !defined(SHA256_FUN_HPP) */
