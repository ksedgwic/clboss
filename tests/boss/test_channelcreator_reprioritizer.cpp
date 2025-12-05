#undef NDEBUG
#include"Boss/Mod/ChannelCreator/Reprioritizer.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ln/NodeId.hpp"
#include"Net/IPAddr.hpp"
#include"Net/IPAddrOrOnion.hpp"
#include"Net/IPBinnerBySubnet.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<map>
#include<stdexcept>
#include<vector>

namespace {

class DummySigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
		throw std::logic_error("Not expected to use.");
	}
	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) override {
		throw std::logic_error("Not expected to use.");
	}
	Sha256::Hash
	get_privkey_salted_hash( std::uint8_t salt[32]
			       ) override {
		/* Dummy.  */
		auto hash = Sha256::Hash();
		hash.from_buffer(salt);
		return hash;
	}
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const& tweak
			) override {
		throw std::logic_error("Not expected to use.");
	}
};
auto dummy_signer = DummySigner();

/* Nodes.  */
auto A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");

}

int main() {
	auto binner = Util::make_unique<Net::IPBinnerBySubnet>();

	auto node_ip = std::map<Ln::NodeId, Net::IPAddrOrOnion>();
	auto get_node_addr = [&](Ln::NodeId n) {
		auto it = node_ip.find(n);
		if (it == node_ip.end())
			return Ev::lift(std::unique_ptr<Net::IPAddrOrOnion>());
		else
			return Ev::lift(Util::make_unique<Net::IPAddrOrOnion>(it->second));
	};
	auto current_peers = std::vector<Ln::NodeId>();
	auto get_peers = [&]() { return Ev::lift(current_peers); };

	/* Object under test.  */
	Boss::Mod::ChannelCreator::Reprioritizer reprioritizer
		( dummy_signer
		, std::move(binner)
		, std::move(get_node_addr)
		, std::move(get_peers)
		);

	typedef std::map<Ln::NodeId, Net::IPAddrOrOnion> IpTableT;
	typedef Net::IPAddrOrOnion Ip;
	typedef std::vector<std::pair<Ln::NodeId, Ln::NodeId>> ResT;

	auto code = Ev::lift().then([&]() {

		/* No known addresses, priority should be same.  */
		return reprioritizer.reprioritize({{A, A}, {B, B}, {C, C}, {D, D}});
	}).then([&](ResT res) {
		auto exp = ResT{{A, A}, {B, B}, {C, C}, {D, D}};
		assert(res == exp);

		/* Assign same address to A and B, unique ones to C and D.  */
		node_ip = IpTableT{ {A, Ip("1.1.1.1")}
				  , {B, Ip("1.1.1.1")}
				  , {C, Ip("2001:db8::2:1")}
				  , {D, Ip("protonirockerxow.onion")}
				  };
		return reprioritizer.reprioritize({{A, A}, {B, B}, {C, C}, {D, D}});
	}).then([&](ResT res) {
		/* B should be de-prioritized.  */
		auto exp = ResT{{A, A}, {C, C}, {D, D}, {B, B}};
		assert(res == exp);

		/* Same as above but A is an existing peer.  */
		current_peers = {A};
		return reprioritizer.reprioritize({{B, B}, {C, C}, {D, D}});
	}).then([&](ResT res) {
		/* B should be de-prioritized, A is not in proposals list.  */
		auto exp = ResT{{C, C}, {D, D}, {B, B}};
		assert(res == exp);

		/* Assign same address to A, B, and C.  */
		node_ip = IpTableT{ {A, Ip("1.1.1.1")}
				  , {B, Ip("1.1.1.1")}
				  , {C, Ip("1.1.1.1")}
				  , {D, Ip("protonirockerxow.onion")}
				  };
		current_peers = {};
		return reprioritizer.reprioritize({{A, A}, {B, B}, {C, C}, {D, D}});
	}).then([&](ResT res) {
		/* B and C should be de-prioritized.
		 * A remains high priority since it was there first and
		 * did not have same address as existing peer.
		 */
		auto exp = ResT{{A, A}, {D, D}, {B, B}, {C, C}};
		assert(res == exp);

		(void) reprioritizer;
		return Ev::lift(0);
	});
	return Ev::start(code);;
}
