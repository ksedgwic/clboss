#include"Boss/Signer.hpp"
#include"Ev/Io.hpp"
#include"Net/Fd.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/Hasher.hpp"
#include"Sqlite3.hpp"
#include"Util/Rw.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<basicsecure.h>
#include<errno.h>
#include<fcntl.h>
#include<stdexcept>
#include<string.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<unistd.h>

namespace {

/* The actual signer object returned.  */
class ActualSigner : public Secp256k1::SignerIF {
private:
	Secp256k1::PrivKey const sk;

public:
	ActualSigner() =delete;
	ActualSigner(ActualSigner&&) =delete;
	ActualSigner(ActualSigner const&) =delete;
	explicit
	ActualSigner(Secp256k1::PrivKey const& sk_) : sk(sk_) { }

	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
		return tweak * Secp256k1::PubKey(sk);
	}
	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) override {
		return Secp256k1::Signature::create(tweak * sk, m);
	}
	Sha256::Hash
	get_privkey_salted_hash(std::uint8_t salt[32]) override {
		auto hasher = Sha256::Hasher();
		hasher.feed(salt, 32);
		std::uint8_t buf[32];
		sk.to_buffer(buf);
		hasher.feed(buf, 32);
		basicsecure_clear(buf, sizeof(buf));
		return std::move(hasher).finalize();
	}
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const& tweak
			) override {
		return Secp256k1::KeyPair(tweak * sk);
	}
};


void unlink_noerr(std::string const& filename) {
	auto my_errno = errno;
	unlink(filename.c_str());
	errno = my_errno;
}

/* Attempt to create the privkey file.  */
void try_create_privkey_file( std::string const& privkey_filename
			    , Secp256k1::Random& random
			    ) {
	auto fd = Net::Fd(open( privkey_filename.c_str()
			      , O_CREAT|O_EXCL|O_WRONLY
			      /* Readable only by owner,
			       * not writeable/executable at all
			       */
			      , 0400
			      ));
	if (!fd && errno == EEXIST)
		/* Benign failure, file already exists.  */
		return;
	if (!fd)
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Boss::Signer: creat(\"") +
			privkey_filename +
			"\"): " + strerror(errno)
		);
	/* Create the privkey.  */
	auto sk = Secp256k1::PrivKey(random);
	/* Try writing it.  */
	auto res = Util::Rw::write_all(fd.get(), &sk, sizeof(sk));
	if (!res) {
		unlink_noerr(privkey_filename);
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Boss::Signer: write(\"") +
			privkey_filename +
			"\"): " + strerror(errno)
		);
	}
}

/* Read private key from given file.  */
Secp256k1::PrivKey
read_privkey_file(std::string const& privkey_filename) {
	auto fd = Net::Fd(open(privkey_filename.c_str(), O_RDONLY));
	if (!fd)
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Boss::Signer: open(\"") +
			privkey_filename +
			"\"): " + strerror(errno)
		);

	auto sk = Secp256k1::PrivKey();
	auto size = sizeof(sk);
	auto res = Util::Rw::read_all(fd.get(), &sk, size);
	if (!res)
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Boss::Signer: read(\"") +
			privkey_filename +
			"\"): " + strerror(errno)
		);
	if (size != sizeof(sk))
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Boss::Signer: read(\"") +
			privkey_filename +
			"\"): unexpected end-of-file"
		);

	return sk;
}

}

namespace Boss {

class Signer::Impl {
private:
	std::string privkey_filename;
	Secp256k1::Random& random;
	Sqlite3::Db db;

	Secp256k1::PrivKey sk;

public:
	Impl( std::string const& privkey_filename_
	    , Secp256k1::Random& random_
	    , Sqlite3::Db db_
	    ) : privkey_filename(privkey_filename_)
	      , random(random_)
	      , db(std::move(db_))
	      { }

	static
	Ev::Io<std::unique_ptr<Secp256k1::SignerIF>>
	construct(std::shared_ptr<Impl> self) {
		return Ev::lift().then([self]() {
			try_create_privkey_file( self->privkey_filename
					       , self->random
					       );
			self->sk = read_privkey_file(self->privkey_filename);
			return self->db.transact();
		}).then([self](Sqlite3::Tx tx) {
			/* Create table if needed.  */
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS "Boss::Signer"
				( id INTEGER PRIMARY KEY
				, pubkey TEXT NOT NULL
				);
			)QRY");
			/* Check if we already have pubkey+tweak.  */
			auto fetch = tx.query(R"QRY(
			SELECT pubkey FROM "Boss::Signer"
			 WHERE id = 0
			     ;
			)QRY").execute();
			auto found = false;
			auto pubkey = Secp256k1::PubKey();
			for (auto& r : fetch) {
				found = true;
				pubkey = Secp256k1::PubKey(
					r.get<std::string>(0)
				);
			}
			if (!found) {
				/* No entry yet, insert it.  */
				pubkey = Secp256k1::PubKey(self->sk);
				tx.query(R"QRY(
				INSERT INTO "Boss::Signer"
				VALUES( 0
				      , :pubkey
				      )
				)QRY")
					.bind( ":pubkey"
					     , Util::stringify(pubkey)
					     )
					.execute()
					;
			} else {
				/* Check it matches.  */
				auto actual_pk = Secp256k1::PubKey(self->sk);
				if (actual_pk != pubkey)
					throw Util::BacktraceException<std::runtime_error>(
						std::string("Boss::Signer: ") +
						self->privkey_filename +
						": not matched to database!"
					);
			}

			tx.commit();

			auto signer = Util::make_unique<ActualSigner>(
				self->sk
			);
			auto rv = std::unique_ptr<Secp256k1::SignerIF>(
				std::move(signer)
			);

			return Ev::lift(std::move(rv));
		});
	}
};

Signer::Signer(Signer&&) =default;
Signer& Signer::operator=(Signer&&) =default;
Signer::~Signer() =default;

Signer::Signer( std::string const& privkey_filename
	      , Secp256k1::Random& random
	      , Sqlite3::Db db
	      ) : pimpl(Util::make_unique<Impl>( privkey_filename
					       , random
					       , std::move(db)
					       ))
		{ }

Ev::Io<std::unique_ptr<Secp256k1::SignerIF>>
Signer::construct()&& {
	auto my_pimpl = std::shared_ptr<Impl>(std::move(pimpl));
	return Impl::construct(my_pimpl);
}

}
