#ifndef SHA256_HASHERSTREAM_HPP
#define SHA256_HASHERSTREAM_HPP

#include<iostream>
#include<memory>

namespace Sha256 { class Hash; }
namespace Tag { enum tap : uint8_t; }

namespace Sha256 {

/** class Sha256::HasherStream
 *
 * @brief an `std::ostream` where the data
 * written is put into a SHA256 hasher.
 *
 * @desc use the member functions `finalize`
 * (on rvalue reference, i.e. with `std::move`)
 * or `get_hash` to get the hash.
 * `finalize` is more efficient as it does not
 * replicate the hasher state, but deletes it
 * immediately.
 */
class HasherStream;

namespace Detail {

class HasherStreamBuf : public std::basic_streambuf<char> {
private:
	using Base = std::basic_streambuf<char>;
	using char_type = typename Base::char_type;
	using int_type = typename Base::int_type;

	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HasherStreamBuf(HasherStreamBuf const&) =delete;
	HasherStreamBuf();
	HasherStreamBuf(HasherStreamBuf&&);
	~HasherStreamBuf();

	int_type overflow(int_type) override;

	Hash finalize()&&;
	Hash get_hash() const;
	void tag(Tag::tap);
};

/* Base class ensures buffer is constructed before ostream is.  */
class HasherStreamBase {
protected:
	std::unique_ptr<HasherStreamBuf> buf;
	HasherStreamBase();
	HasherStreamBase(HasherStreamBase&&) =default;
};

}

class HasherStream : private Detail::HasherStreamBase, public std::ostream {
public:
	HasherStream() : std::ostream(buf.get()) { }
	/* HasherStream(HasherStream&&) =default; */

	/* initialized with a bip340 tagged hash.  */
	HasherStream(Tag::tap tag)
		: std::ostream(buf.get()) { buf->tag(tag); }

	Hash finalize()&& {
		return std::move(*buf).finalize();
	}
	Hash get_hash() const {
		return buf->get_hash();
	}
};

}

#endif /* !defined(SHA256_HASHERSTREAM_HPP) */
