#include"Sha256/Hash.hpp"
#include"Sha256/Hasher.hpp"
#include"Sha256/HasherStream.hpp"
#include"Secp256k1/tagged_hashes.hpp"
#include"Util/make_unique.hpp"
#include<string.h>

namespace Sha256 { namespace Detail {

class HasherStreamBuf::Impl {
public:
	Sha256::Hasher hasher;
	char buf[64];
};

HasherStreamBuf::HasherStreamBuf() : pimpl(Util::make_unique<Impl>()) {
	Base::setp(pimpl->buf, pimpl->buf + sizeof(pimpl->buf));
}
HasherStreamBuf::HasherStreamBuf(HasherStreamBuf&&) =default;
HasherStreamBuf::~HasherStreamBuf() =default;

HasherStreamBuf::int_type
HasherStreamBuf::overflow(HasherStreamBuf::int_type ch) {
	/* Feed buffer to the hasher.  */
	pimpl->hasher.feed(pbase(), pptr() - pbase());
	/* Reset space.  */
	Base::setp(pimpl->buf, pimpl->buf + sizeof(pimpl->buf));
	/* If char is not eof, write it to the buffer.  */
	if (ch != std::char_traits<char>::eof()) {
		*pbase() = (char_type) ch;
		pbump(1);
	}
	return ch;
}

Hash HasherStreamBuf::finalize()&& {
	/* Flush.  */
	overflow(std::char_traits<char>::eof());
	/* Extract.  */
	return std::move(pimpl->hasher).finalize();
}
Hash HasherStreamBuf::get_hash() const {
	/* Copy hasher.  */
	auto tmp = pimpl->hasher;
	/* Feed it any data.  */
	tmp.feed(pbase(), pptr() - pbase());
	/* Finalize the temporary.  */
	return std::move(tmp).finalize();
}
void HasherStreamBuf::tag(Tag::tap tag) {
	/* Load buffer with bip340 tagged hashes.  */
	if ((uint8_t) tag > 3)
		return; // TODO throw exception

	auto *ptagtext = Tag::str(tag);
	auto tmp = Hasher();
	tmp.feed(ptagtext, strlen(ptagtext));
	auto hashedtag = std::move(tmp).finalize();
	uint8_t buffer[32];
	hashedtag.to_buffer(buffer);
	pimpl->hasher.feed(buffer, 32);
	pimpl->hasher.feed(buffer, 32);

}

HasherStreamBase::HasherStreamBase()
	: buf(Util::make_unique<HasherStreamBuf>()) { }

}}
