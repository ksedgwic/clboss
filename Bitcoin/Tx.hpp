#ifndef BITCOIN_TX_HPP
#define BITCOIN_TX_HPP

#include<cstdint>
#include"Bitcoin/TxIn.hpp"
#include"Bitcoin/TxOut.hpp"

namespace Bitcoin { class TxId; }

namespace Bitcoin {

/** struct Bitcoin::Tx
 *
 * @brief represents a complete Bitcoin
 * transaction.
 */
class Tx {
public:
	std::uint32_t nVersion;
	std::vector<TxIn> inputs;
	std::vector<TxOut> outputs;
	std::uint32_t nLockTime;

	Bitcoin::TxId get_txid() const;

	Tx() : nVersion(2), nLockTime(0) { }

	explicit
	Tx(std::string const&);
	explicit
	operator std::string() const;

	bool operator==(Tx const& o) const {
		return nVersion == o.nVersion
		    && inputs == o.inputs
		    && outputs == o.outputs
		    && nLockTime == o.nLockTime
		     ;
	}
	bool operator!=(Tx const& o) const {
		return !(*this == o);
	}
};

}

std::ostream& operator<<(std::ostream&, Bitcoin::Tx const&);
std::istream& operator>>(std::istream&, Bitcoin::Tx&);

#endif /* !defined(BITCOIN_TX_HPP) */
